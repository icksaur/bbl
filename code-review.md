# BBL Code Quality Review

**Date:** 2025-07-14
**Scope:** bbl.h, bbl.cpp, jit.cpp, compiler.cpp, chunk.h, vm.h, vm.cpp, jit.h, test_bbl.cpp
**Total lines reviewed:** ~17,200

---

## Executive Summary

BBL is a hand-rolled language runtime with a lexer, parser, bytecode compiler, interpreter, and x86-64 JIT — all in ~10k lines of core C++. The implementation is impressive in scope but has reached a point where the codebase's growth model (copy-paste-evolve) is becoming a serious liability. The biggest structural problem is **massive code duplication between the interpreter, JIT helpers, and stdlib**, where the same logic is implemented 2-3 times with subtle divergences. This is a defect factory.

**Severity scale:** 🔴 Critical (bugs now or soon) | 🟠 High (significant maintainability debt) | 🟡 Medium | ⚪ Low

---

## 🔴 CRITICAL: Correctness & Safety Issues

### 1. SlabAllocator destructor is a no-op — memory leak and missing destructors
**File:** `bbl.h:566-574`

```cpp
~SlabAllocator() {
    for (auto& slab : slabs) {
        for (size_t i = 0; i < slab->used; i++) {
            auto* p = reinterpret_cast<T*>(slab->storage + sizeof(T) * i);
            (void)p;  // ← does literally nothing
        }
    }
}
```

The destructor iterates over every allocated object and then **casts it to `(void)` — discarding it**. No destructors are called. No memory is freed. The slab storage itself is freed by `unique_ptr<Slab>`, but for `SlabAllocator<BblTable>` and `SlabAllocator<BblClosure>`, objects that have been `free()`'d back to the free list have already been destructed, but objects that are still live when the allocator dies will have their destructors skipped. This is undefined behavior for objects with non-trivial destructors (BblTable has a destructor that frees `buckets`, `order`, and `array`).

The `BblState` destructor (`bbl.cpp:663-697`) manually walks the GC lists and destructs everything, which partially compensates — but any object on the free list that was destructed by `SlabAllocator::free()` and then the slab is destroyed is fine. The real concern is that slabs hold raw memory for objects that may or may not still be logically alive. This destructor should either call destructors on live objects or document that `BblState::~BblState()` handles it.

### 2. `g_currentBblState` global state — thread safety violation
**File:** `bbl.cpp:23, 108, 119-121, 608, 1191, 1203, 1216`

```cpp
static thread_local BblState* g_currentBblState = nullptr;
```

`thread_local` prevents cross-thread races, but `BblTable::set()` uses `g_currentBblState` for write barriers (`bbl.cpp:108`). When a child state runs on a worker thread, it sets `g_currentBblState` in its `exec()` call. But if the parent state's tables are shared (e.g., struct/userdata descriptors via pointers), calling `set()` on those tables from either thread could use the wrong `BblState*` for write barriers, corrupting the GC's remembered set.

### 3. OP_MOD doesn't type-check operands
**File:** `vm.cpp:138-141`

```cpp
case OP_MOD: {
    BblValue& rb = R(B); BblValue& rc = R(C);
    if (rc.intVal() == 0) throw BBL::Error{"modulo by zero"};
    R(A) = BblValue::makeInt(rb.intVal() % rc.intVal());
    break;
}
```

Calls `intVal()` without checking `type() == Int`. If either operand is a float, string, or null, `intVal()` reinterprets the NaN-boxed bits as a sign-extended 48-bit integer — silent data corruption. Every other arithmetic op checks types first.

### 4. `OP_EXEC` parses but discards the AST
**File:** `vm.cpp:658-664`

```cpp
case OP_EXEC: {
    if (R(B).type() != BBL::Type::String) throw BBL::Error{"exec: argument must be string"};
    BblLexer lexer(R(B).stringVal()->data.c_str());
    auto nodes = parse(lexer);        // ← parsed, never used
    R(A) = state.execExpr(R(B).stringVal()->data);  // ← parses AGAIN inside execExpr
    break;
}
```

The AST from `parse(lexer)` is computed and immediately thrown away. `execExpr` parses the same source again internally. This is a bug that wastes work and was likely left behind from a refactor.

### 5. `execfile` vs `execfileExpr` — BBL_PATH search logic duplicated with subtle difference
**File:** `bbl.cpp:1274-1351`

`execfile()` increments `execDepth` **before** resolving the path. `execfileExpr()` resolves the path **before** incrementing `execDepth`. If path resolution throws, `execfile` has already incremented the depth counter (which is decremented in the non-exception path), leaking a depth count. Meanwhile `execfileExpr` would skip the increment entirely. Both functions are ~40 lines that differ only in whether they return a value.

### 6. BblTable uses `calloc`/`free` but `BblValue` has non-trivial padding concerns
**File:** `bbl.cpp:61, 71, 91-99`

`BblTable::arrayGrow()` uses `calloc` + `memcpy` for the array part. Since `BblValue` is a POD (just a `uint64_t`), this is technically fine. But `BblTable::Entry` contains two `BblValue` objects and `calloc` zeros them. The zero-bits sentinel (`EMPTY_KEY = 0`) works because `BblValue` with `bits=0` is not a valid null (null is `TAG_NULL`). This is clever but fragile — if `BblValue`'s representation ever changes, this breaks silently. No static_assert guards this invariant.

---

## 🟠 HIGH: Maintainability Defects

### 7. Massive method dispatch duplication: vm.cpp OP_MCALL vs jit.cpp jitMcall
**File:** `vm.cpp:502-638` (~136 lines) and `jit.cpp:218-500` (~282 lines)

The entire method dispatch logic for tables, vectors, strings, binaries, and userdata is implemented **twice** — once in the interpreter and once in the JIT helper. They have diverged:

- **JIT** has `MID_PAD_LEFT`, `MID_PAD_RIGHT`, `MID_JOIN`, `MID_AS`, `MID_SET_AS` dispatch for strings/binaries. The **interpreter does not** — those methods silently fall through to "unknown method" in the VM path.
- The JIT uses `methodId` switch for strings (fast path), while the interpreter uses pointer equality checks against `state.m.*`.
- The JIT has additional bounds checking in binary methods (`binary.set`, `binary.at`) that the interpreter lacks.

This is the single highest-impact maintainability problem. Every new method must be added in 2+ places, and divergence is already happening.

### 8. `gcMark` and `gcMarkGen0` are copy-pasted with ~5 lines different
**File:** `bbl.cpp:818-954` (137 lines)

These two ~68-line functions are nearly identical. `gcMarkGen0` adds `if (obj->old) return;` checks. This should be a single template or parameterized function. Every change to GC marking must be made twice and kept in sync.

### 9. `readField`/`writeField` — massive switch statements for every CType
**File:** `bbl.cpp:1637-1812` (175 lines)

`readField` is 65 lines of switch cases that could be a lookup table + `memcpy`. `writeField` is 105 lines that repeat the same pattern: check type, cast, memcpy. These functions will grow linearly with every new CType added. Consider a table-driven approach.

### 10. `readVecElem`/`writeVecElem`/`packValue` — triple duplication
**File:** `bbl.cpp:1836-1949` (113 lines)

Three functions that all switch on `elemTypeTag` and do essentially the same memcpy patterns with different error handling. `packValue` is `writeVecElem` + resize. Factor out a common "convert BblValue to bytes" and "convert bytes to BblValue" primitive.

### 11. `bbl.cpp` is 4,137 lines — at least 4 files crammed into one
**File:** `bbl.cpp`

This file contains:
- Lexer implementation (~300 lines)
- Parser (~100 lines)
- BblState / GC / allocation (~600 lines)
- Struct/vector field read/write (~400 lines)
- Print/Str/Type stdlib (~200 lines)
- FileIO stdlib (~200 lines)
- Math stdlib (~100 lines)
- OS stdlib (~350 lines)
- Networking stdlib (~400 lines)
- Core stdlib (JSON, sort, random, etc.) (~400 lines)
- Child states / messaging (~500 lines)
- Sandbox, atomic buffers, channels, NREPL (~500 lines)

The stdlib sections (addPrint, addFileIo, addMath, addOs, addNet, addCore, addChildStates, addSandbox) are completely independent and should be separate files. Having 4,137 lines in one file means you can't grep for a function without wading through unrelated code, and compile times are needlessly long since changing any stdlib function recompiles everything.

### 12. `jit.cpp` is 3,988 lines — two very different things in one file
**File:** `jit.cpp`

This file contains:
- ~700 lines of C helper functions (jitCall, jitMcall, jitGetField, etc.)
- ~500 lines of x86-64 machine code emission helpers (emitAdd, emitSub, emitPrologue, etc.)
- ~700 lines of `jitCompile` (main whole-function JIT compiler)
- ~1000 lines of trace recording + optimization
- ~700 lines of trace JIT compilation

The C helpers should be a separate file. The trace system (record, optimize, compile) should be a separate file.

### 13. `stringNoArgMethod` is orphaned from main dispatch
**File:** `bbl.cpp:1141-1175`

This function exists in bbl.cpp but is only used for dot-access syntax, not for the interpreter's OP_MCALL dispatch or the JIT's jitMcall. This means `"hello".upper` (dot access) might work but `"hello":upper` (method call) takes a different code path. The logic is not shared.

### 14. `getArgType` / `getIntArg` / `getFloatArg` / `getBoolArg` / `getStringArg` / `getBinaryArg` — boilerplate that should be templated
**File:** `bbl.cpp:1465-1529` (65 lines)

Six functions with identical structure: bounds check, type check, extract. This is a textbook case for a template + specialization pattern, or at minimum a helper macro.

### 15. `pushInt` / `pushFloat` / `pushBool` / `pushString` / `pushNull` / `pushTable` / `pushBinary` — same pattern 7 times
**File:** `bbl.cpp:1533-1574` (42 lines)

Each does `returnValue = BblValue::makeX(val); hasReturn = true; returnValues.push_back(returnValue);`. Seven identical patterns.

---

## 🟡 MEDIUM: Design & Naming Issues

### 16. `returnValue` + `hasReturn` + `returnValues` + `returnCount` — four fields for one concept
**File:** `bbl.h:617-620`

```cpp
BblValue returnValue;
bool hasReturn = false;
int returnCount = 0;
std::vector<BblValue> returnValues;
```

These four fields encode the return convention for C functions and must be kept in sync. Some C functions set `returnValue` and `hasReturn` directly. Others call `pushInt` etc. which sets all three. The JIT's `jitCall` checks `returnCount > 1` to handle multiple returns. This is error-prone: you can set `hasReturn = true` without pushing to `returnValues`, or push to `returnValues` without setting `hasReturn`. A single `std::optional<std::vector<BblValue>>` or a proper ReturnValue struct would be clearer.

### 17. `callArgs` — mutable shared state for function calls
**File:** `bbl.h:616`

C function arguments are passed via `state.callArgs`, a mutable vector on BblState. If a C function calls back into BBL (which calls another C function), the inner call overwrites `callArgs`. This works only because each C function reads its args before calling anything else, but it's a latent bug waiting to happen if any C function ever saves a reference to `callArgs` across a nested call.

### 18. Mixed naming conventions
**File:** Various

- C++ methods: `camelCase` (e.g., `allocString`, `execExpr`)
- BBL builtins: `kebab-case` (e.g., `"exec-file"`, `"type-of"`)
- Struct fields: `camelCase` (e.g., `elemType`, `gcType`)
- Enum values: `MID_UPPER`, `OP_LOADK` (SCREAMING_SNAKE)
- C functions: `bblPrint`, `bblOs_getenv` (sometimes prefix, sometimes not)
- GC fields: `marked`, `old`, `dirty` (good short names)
- BblState fields: mixed — `m` for method names cache, `vm` for VM state, `rng` for RNG

The `bblOs_` prefix is inconsistent — some OS functions use it, others don't (`bblFilebytes`, `bblFopen`). File IO helpers should be `bblFileIo_*` or similar.

### 19. `BblState` is a god object — 100+ fields
**File:** `bbl.h:576-816` (240 lines)

`BblState` contains: GC state, intern table, slab allocators, VM pointer, struct descriptors, userdata descriptors, call args/return values, backtrace state, step limit, RNG, script directory, sandbox flag, exec depth, module cache, method name cache, print capture, termination flag, child state handle, last recv payload, symbol tables, and more.

This violates SRP. At minimum, the GC state, the call convention state, and the child-state messaging state should be separate structs.

### 20. `Token` and `AstNode` carry all variant data as fields
**File:** `bbl.h:393-476`

```cpp
struct Token {
    TokenType type;
    int64_t intVal = 0;
    double floatVal = 0;
    bool boolVal = false;
    std::string stringVal;
    std::vector<uint8_t> binaryData;
    const char* binarySource = nullptr;
    size_t binarySize = 0;
    bool isCompressed = false;
    int line = 1;
    int sourceStart = 0;
    int sourceEnd = 0;
};
```

Every Token allocates a `std::string` and `std::vector<uint8_t>` regardless of type. A symbol token carries dead `floatVal`, `boolVal`, `binaryData` fields. Same for `AstNode`. Use `std::variant` or a union to reduce memory waste, especially since AST nodes are allocated in bulk during parsing.

### 21. Hardcoded type resolution duplicated in 3 places
**File:** `vm.cpp:396-409`, `jit.cpp:503-518`, `compiler.cpp:715-721`

The mapping from type name string to `{CType, size}` is duplicated in three locations (vector creation in VM, vector creation in JIT, struct definition in compiler). Each has slightly different supported types. For example, the JIT's `jitVector` supports `int16` and `int8`/`uint8` only via `resolveTypeName`, while the VM's `OP_VECTOR` handler uses a different inline mapping that supports more types.

### 22. `#include <lz4frame.h>` in the middle of bbl.cpp
**File:** `bbl.cpp:722`

An `#include` directive at line 722, in the middle of the file between function definitions. All includes should be at the top of the file.

---

## ⚪ LOW: Minor Issues

### 23. Dead code: `ObjKind` enum is never used
**File:** `bbl.h:101`

```cpp
enum class ObjKind : uint8_t { Table, Vector, Struct, Binary, UserData };
```

Never referenced anywhere in the codebase. `GcType` is used instead.

### 24. Dead forward declarations
**File:** `bbl.cpp:3738-3741`

```cpp
static int bblChildPost(BblState* bbl);
static int bblChildRecv(BblState* bbl);
static int bblChildRecvVec(BblState* bbl);
```

These are forward-declared twice — once at line 3424-3427 and again at 3738-3741.

### 25. `bblSelect` and `bblSelectTimeout` are copy-pasted (~50 lines each)
**File:** `bbl.cpp:4011-4063` and `bbl.cpp:4065-4116`

These two functions differ only in the use of `wait` vs `wait_for`. Factor out the common pattern.

### 26. `nrepl-exec` and `nrepl-poll` have duplicated eval+capture logic
**File:** `bbl.cpp:3489-3526` and `bbl.cpp:3528-3591`

Both functions do the same pattern: save env/capture, exec code, catch errors, build result table. ~30 lines duplicated.

### 27. `BblFn` struct carries AST interpreter data but is barely used
**File:** `bbl.h:355-363`

`BblFn` has `params`, `body` (AST nodes), `captures`, `slotIndex`, `paramSlotStart` — all fields for a tree-walking interpreter. With the bytecode compiler + JIT, this type seems vestigial. `BblClosure` (in vm.h) holds the bytecode `Chunk`. If the tree-walker is gone, `BblFn` should be removed or minimized.

### 28. `OP_ADDK` constant `1` added repeatedly in each loop
**File:** `compiler.cpp:532`

```cpp
size_t oneIdx = cs.chunk.addConstant(BblValue::makeInt(1));
```

Every `each` loop adds a new constant `1` to the chunk's constant pool. The constant pool should be deduped, or the compiler should cache common constants.

### 29. No `static_assert` on `sizeof(BblValue)`
**File:** `bbl.h:238`

The entire NaN-boxing scheme depends on `sizeof(BblValue) == 8` and `sizeof(uint64_t) == 8`. There's no static assertion to catch platform or compiler changes. Add `static_assert(sizeof(BblValue) == 8)`.

### 30. Test file is 6,274 lines with no organization
**File:** `tests/test_bbl.cpp`

594 tests in a single file with hand-rolled test macros. No test grouping, no filtering, no parameterized tests. Adding a test requires adding both a `TEST()` definition and a `RUN()` call. If you add one without the other, there's no compilation error — just a silently skipped test.

---

## Prioritized Action Items

| Priority | Action | Impact | Effort |
|----------|--------|--------|--------|
| 🔴 1 | Fix OP_MOD type checking (vm.cpp:138) | Correctness | Trivial |
| 🔴 2 | Remove dead parse in OP_EXEC (vm.cpp:660) | Correctness | Trivial |
| 🔴 3 | Fix SlabAllocator destructor or document invariant | Safety | Low |
| 🔴 4 | Add static_assert(sizeof(BblValue)==8) | Safety | Trivial |
| 🟠 5 | Unify method dispatch (VM + JIT) into shared code | Maintainability | High |
| 🟠 6 | Merge gcMark/gcMarkGen0 into parameterized function | Maintainability | Low |
| 🟠 7 | Extract stdlib into separate files (fileio, os, net, etc.) | Maintainability | Medium |
| 🟠 8 | Consolidate execfile/execfileExpr | Maintainability | Low |
| 🟠 9 | Factor out readField/writeField into table-driven | Maintainability | Medium |
| 🟡 10 | Consolidate type resolution tables | Maintainability | Low |
| 🟡 11 | Clean up BblState — extract GC state, call state | Design | High |
| 🟡 12 | Remove dead code (ObjKind, duplicate forward decls, BblFn fields) | Cleanliness | Trivial |
| ⚪ 13 | Adopt a real test framework (Catch2, doctest) | Testing | Medium |

---

## Summary Statistics

| File | Lines | Assessment |
|------|-------|------------|
| bbl.h | 836 | Oversized header. God object (BblState). Dead enum. SlabAllocator destructor bug. |
| bbl.cpp | 4,137 | **Critical.** At least 8 separable concerns crammed together. Massive duplication. |
| jit.cpp | 3,988 | Two files in one (C helpers + x86 codegen). Method dispatch duplicated from VM. |
| compiler.cpp | 940 | Reasonable size. compileList is long but manageable. Minor duplication in type tables. |
| chunk.h | 167 | Clean. Well-structured instruction encoding. |
| vm.h | 77 | Clean. |
| vm.cpp | 745 | OP_MOD type-check bug. OP_EXEC dead code. Method dispatch should be shared. |
| jit.h | 68 | Clean. |
| test_bbl.cpp | 6,274 | Massive single file. Hand-rolled framework. No test discovery. |

**Bottom line:** The codebase works, but the copy-paste growth model has created a situation where adding a new method requires changes in 2-3 places that are already diverging. Fix the duplication before it gets worse.
