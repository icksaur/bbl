# BBL Implementation Notes

Internal architecture, performance characteristics, and design decisions.

---

## Architecture

BBL is JIT-only: `exec()` → parse → compile → JIT-compile → execute native code.
There is no interpreter fallback — all code runs as x86-64 machine code.

**Pipeline:**
```
Source text → Lexer (BblLexer) → Parser (parseExpr) → Compiler (compile)
    → Bytecode (Chunk) → JIT Compiler (jitCompile) → Native x86-64
```

**Files:**
- `bbl.h` — data types, BblState, public API
- `bbl.cpp` — lexer, parser, stdlib, GC
- `compiler.cpp` — AST → bytecode compiler
- `chunk.h` — bytecode encoding (32-bit instructions, iABC/iABx/iAsBx)
- `jit.cpp` — copy-and-patch JIT, trace JIT, register allocator, inline caches
- `vm.cpp` — trace side-exit handler

---

## Value Representation

NaN-boxed 8-byte `BblValue`. IEEE 754 doubles stored inline. Non-double types
use the NaN payload space:

| Tag | Bits 50:48 | Payload | Type |
|-----|-----------|---------|------|
| TAG_NULL | 0 | — | null |
| TAG_BOOL | 1 | 0 or 1 | bool |
| TAG_INT | 2 | 48-bit signed | int |
| TAG_CLOSURE | 3 | pointer | closure |
| TAG_CFN | 4 | function pointer | C function |
| TAG_FN | 5 | pointer | BBL function |
| TAG_STRING | 6 | pointer | string |
| TAG_OBJECT | 7 | pointer | table/vector/struct/binary/userdata |

Integers beyond ±2^47 auto-promote to double.

---

## Memory Model

### Generational Garbage Collection

Two-generation nursery/tenured collector.

**Object layout:**
```cpp
struct GcObj {
    GcType gcType;    // 1 byte
    bool marked;      // 1 byte
    bool old;         // 1 byte (false=nursery, true=tenured)
    bool dirty;       // 1 byte (write barrier dedup)
    GcObj* gcNext;    // 8 bytes — intrusive linked list
};
```
Total 16 bytes (unchanged from pre-generational layout due to padding).
All GC-managed types inherit from GcObj. Two separate lists:
`nurseryHead` (Gen0) and `tenuredHead` (Gen1).

**Gen0 (minor) collection** — `gcMinor()`:
1. Mark from roots using generation-aware traversal (stops at Gen1 objects)
2. Mark from remembered set (Gen1 objects with cross-generation references)
3. Sweep nursery list only — free unmarked, promote survivors to tenured
4. Rebuild remembered set

**Gen1 (full) collection** — `gcFull()`:
Full mark-sweep of both lists. Infrequent.

**Write barriers:** Every store into a Gen1 object checks if the value is a
Gen0 GC pointer. If so, the container is added to the remembered set via a
`dirty` bit (dedup). The JIT emits inline barriers after table array stores:
```asm
test byte [container+2], 1    ; check old flag
jz .skip                      ; young → no barrier (99% fast path)
call jitWriteBarrier           ; slow path
```

**Safe points for GC triggers:**
- Start of `exec()` / `execExpr()`
- `jitTable` helper (before table allocation)
- VM trace side-exit handler (vm.cpp)
- GC is paused during bytecode compilation (`compile()`)

**Slab allocators:** Tables and closures use `SlabAllocator<T>` (256-object
slabs with free lists) for reduced allocation overhead.

**Roots:**
- VM register file (`vm->stack`)
- Global symbol table (`vm->globals`)
- Call arguments, return values
- Method name cache (`m.length`, `m.push`, etc.)
- Symbol name table, module cache, current environment

### String Interning

All strings live in a global intern table keyed by content. Duplicate strings
share one allocation. Equality comparison is O(1) via pointer compare. Interned
strings are swept normally — if unreachable, they're removed from the intern
table during GC.

---

## Compilation

### Bytecode Format

32-bit instructions in three encodings:
- **iABC:** `op(8) | A(8) | B(8) | C(8)` — 3-operand
- **iABx:** `op(8) | A(8) | Bx(16)` — constant index
- **iAsBx:** `op(8) | A(8) | sBx(16)` — signed offset (bias 32767)

~90 opcodes covering arithmetic, comparison, control flow, table/vector ops,
function calls, closures, globals, and captures.

### Register Allocation (Compiler)

Simple linear stack: `allocReg()` increments `nextReg`. Locals map to
registers. Temporaries allocated and freed around expressions. Max 255
registers per function (uint8_t). Bounds-checked with error on overflow.

### Compiler Safety

All overflow paths checked at compile time:
- Register exhaustion (>255)
- Constant pool overflow (>65535 for uint16_t, >255 for uint8_t fields)
- Jump distance overflow (±32767)
- Argument count overflow (>255)
- Parser recursion depth (256), compiler recursion depth (512)
- Number parsing via `std::from_chars` (no exception leaks)

---

## JIT Compiler

### Per-Function JIT (Copy-and-Patch)

`jitCompile()` translates bytecode to x86-64 per-instruction. System V ABI.
Register convention: `rbx` = register file, `r12` = BblState*, `r13` = Chunk*,
`r14` = full/light call marker.

**Optimizations:**
- **Tag-preserving arithmetic:** `ADD = a + b - TAG_INT` (5 instructions vs 13)
- **Known-type tracking:** `KnownType` enum (Unknown/Int/Table/Float) per register.
  Eliminates type guards when types are statically known.
- **Register allocator:** Pre-scans bytecode for innermost all-integer loops.
  Unboxes VM registers into hardware registers (r8-r11/rdx/rsi/rdi). Loop body
  operates on raw integers. Prologue/epilogue handle boxing/unboxing.
- **Inline float ops:** `emitFAdd/FSub/FMul/FDiv` using SSE2 `movsd/addsd/mulsd/divsd`.
  NaN-boxed floats ARE raw IEEE 754 — zero unbox overhead.
- **Inline table get/set:** Fibonacci hash probe emitted inline for OP_MCALL
  "get"/"set". Array fast path for integer keys: bounds check + direct load/store.
- **Inline vector:at, string:length:** Type guard + direct field access.
- **Callee inlining:** Speculative callee guard (cmp closure ptr, jne slow path).
  Inline body remapped to caller registers. KnownTypes propagated.

### Trace JIT

Hot loops (OP_LOOP with iteration count ≥ 32) are trace-compiled:
1. **Record:** Execute loop body, recording each instruction + operand types
2. **Optimize:** Eliminate dead stores, sink allocations, DCE
3. **Compile:** Emit specialized native code for the recorded trace
4. **Execute:** Jump to trace code at loop entry. Side-exit on type guard failure.

**Trace optimizations:**
- Allocation sinking (tables that don't escape are eliminated)
- Post-sink DCE (dead arithmetic feeding sunk allocations removed)
- Inline array get/set in trace compiler
- Per-loop trace storage (separate traces per loop, not per function)

---

## Embedding API Highlights

### bbl.call

Invoke pre-compiled closures from C++ without re-parsing:
```cpp
auto fn = bbl.get("my-fn").value();
BblValue result = bbl.call(fn, {BblValue::makeInt(42)});
```

### Multiple Return Values

C functions push multiple values, C++ reads via `getReturnValues()`.

### Batch Constant Registration

`defnTable("Name", {{"Key", value}, ...})` for enum-style constant tables.

### Atomic Double-Buffer

Lock-free SPSC userdata type for audio producer/consumer patterns.

---

## Performance

BBL beats or matches LuaJIT on 7/9 benchmarks and beats Node.js on all 9:

| Benchmark | BBL | LuaJIT | Node.js |
|---|---|---|---|
| loop_arith | 5ms | 8ms | 56ms |
| function_calls | 5ms | 5ms | 41ms |
| gc_pressure | 3ms | 2ms | 23ms |
| table_heavy | 2ms | 2ms | 154ms |
| recursion | 62ms | 68ms | 79ms |
| method_dispatch | 2ms | 4ms | 23ms |
| string_build | 2ms | 181ms | 30ms |
| string_parse | 2ms | 17ms | 51ms |
| closure_capture | 4ms | 5ms | 23ms |

Startup time: ~200µs (BblState ctor + addStdLib + first exec).
