# Copy-and-Patch JIT for BBL
Status: proposed

## Goal

Eliminate interpreter dispatch overhead entirely by compiling bytecode to
native x86-64 machine code at runtime using the copy-and-patch technique.
Each bytecode instruction maps to a pre-compiled native code "stencil" (a
small blob of machine code with holes for operands).  At runtime, the JIT
copies the stencils sequentially into an executable buffer and patches in
the concrete operand values.  No LLVM, no register allocator, no
optimization passes.

Expected result: match or exceed Lua 5.4 on all benchmarks.  Approach
LuaJIT interpreter speed on tight loops.

---

## Background

### Current state

BBL has a stack-based bytecode VM with computed-goto dispatch.  The hot
loop in `loop_arith` executes 13 instructions per iteration.  Each
instruction costs ~2 cycles of dispatch overhead (computed goto on modern
x86).  That's ~26 cycles of pure dispatch per loop iteration, plus the
actual work.

Lua 5.4's register VM does the same work in ~5 instructions (~10 cycles
dispatch).  LuaJIT's interpreter uses hand-written assembly with zero
dispatch overhead in straight-line code.

### What is copy-and-patch

The key insight: the C++ compiler already knows how to generate machine
code for each bytecode handler.  If we compile each handler as a separate
function, we get an object file containing native code plus relocation
records (holes where operands go).  At runtime, we just `memcpy` the
machine code and fill in the holes.

For BBL, we simplify further: instead of parsing object files at build
time, we hand-write small native code templates (stencils) for each
opcode.  Each stencil is a `constexpr uint8_t[]` array with placeholder
bytes that get patched with concrete values at JIT time.

This is sometimes called a "template JIT" or "macro assembler JIT" — the
simplest possible JIT that eliminates dispatch without any optimization.

### Why this approach

| Approach | Dispatch cost | Code quality | Complexity |
|----------|--------------|--------------|------------|
| Switch interpreter | ~5 cycles/op | N/A | Very low |
| Computed goto | ~2 cycles/op | N/A | Low |
| **Copy-and-patch** | **0 cycles/op** | Low (no optimization) | **Medium** |
| Optimizing JIT | 0 cycles/op | High | Very high |

Copy-and-patch eliminates ALL dispatch overhead.  The generated code is a
straight sequence of native instructions — no indirect jumps, no branch
mispredictions, no opcode decoding.  The CPU sees ordinary machine code.

The code quality is "low" in the sense that we don't do register
allocation across instructions (each stencil manages its own stack
accesses).  But for BBL's use case this is fine — the dispatch overhead
was the bottleneck, not the per-instruction code quality.

---

## Design

### Architecture

```
Bytecode (Chunk)
    |
    v
JIT Compiler (jit.cpp)
    |  for each instruction:
    |    1. look up stencil for opcode
    |    2. memcpy stencil into code buffer
    |    3. patch operand holes with concrete values
    |
    v
Executable buffer (mmap'd with PROT_EXEC)
    |
    v
Direct function call → native execution → return result
```

### Stencil format

Each stencil is a struct:

```cpp
struct Stencil {
    const uint8_t* code;    // pre-compiled machine code bytes
    size_t size;            // length in bytes
    struct Patch {
        uint8_t offset;     // byte offset within stencil to patch
        uint8_t type;       // PATCH_IMM32, PATCH_IMM64, PATCH_REL32
        uint8_t operand;    // which operand (0=first, 1=second, etc)
    };
    const Patch* patches;
    size_t patchCount;
};
```

### Calling convention

The JIT'd code uses a fixed register assignment:

```
rbx = BblValue* stackTop   (callee-saved, persistent)
r12 = BblValue* frameSlots (callee-saved, persistent)
r13 = BblState* state      (callee-saved, persistent)
rdi/rsi/rdx = scratch for function args
rax = scratch for return values
```

Each stencil reads/writes the stack through `rbx` (stackTop) and `r12`
(frame base).  Stencils that call C++ helper functions (like `intern()`
for string ops) use the standard System V ABI with `r13` as the state
pointer.

### Example stencils

**OP_CONSTANT (push constants[idx]):**
```nasm
; operand: idx (u16, patched to absolute pointer)
mov  rax, 0x0000000000000000   ; patched: &chunk->constants[idx]
mov  rdx, [rax]                ; load BblValue.intVal (first 8 bytes)
mov  rcx, [rax+8]              ; load BblValue type+flags (next 8 bytes)
mov  [rbx], rdx                ; store to stackTop
mov  [rbx+8], rcx
add  rbx, 24                   ; stackTop++ (sizeof BblValue = 24)
; total: ~25 bytes, 5 instructions
```

**OP_ADD (int fast-path):**
```nasm
sub  rbx, 24                   ; pop b
mov  eax, [rbx+16]             ; b.type
sub  rbx, 24                   ; pop a
mov  ecx, [rbx+16]             ; a.type
cmp  eax, 2                    ; Type::Int == 2
jne  .slow
cmp  ecx, 2
jne  .slow
mov  rax, [rbx]                ; a.intVal
add  rax, [rbx+24]             ; + b.intVal
mov  [rbx], rax                ; result.intVal
mov  dword [rbx+16], 2         ; result.type = Int
add  rbx, 24                   ; push result
jmp  .next                     ; fall through to next stencil
.slow:
; call C++ helper: jitSlowAdd(state, stackTop)
mov  rdi, r13
mov  rsi, rbx
call 0x0000000000000000         ; patched: &jitSlowAdd
add  rbx, 24                   ; helper leaves result at stackTop
.next:
; total: ~60 bytes
```

**OP_GET_LOCAL (push frame->slots[slot]):**
```nasm
; operand: slot (u16, patched to byte offset)
mov  rax, [r12 + 0x0000]       ; patched: slot * 24
mov  rdx, [r12 + 0x0000 + 8]   ; patched: slot * 24 + 8
mov  [rbx], rax
mov  [rbx+8], rdx
add  rbx, 24
; total: ~20 bytes
```

**OP_JUMP_IF_FALSE:**
```nasm
sub  rbx, 24                   ; pop condition
mov  eax, [rbx+16]             ; type
cmp  eax, 0                    ; Null?
je   .falsy
cmp  eax, 1                    ; Bool?
jne  .truthy
cmp  byte [rbx], 0             ; boolVal == false?
je   .falsy
.truthy:
jmp  0x00000000                 ; patched: offset to next instruction (fall through)
.falsy:
jmp  0x00000000                 ; patched: offset to jump target
; total: ~35 bytes
```

**OP_CALL (call function):**
```nasm
; Complex — delegates to C++ helper
mov  rdi, r13                  ; state
mov  rsi, rbx                  ; stackTop
mov  edx, 0x00                 ; patched: argc
call 0x0000000000000000         ; patched: &jitCallValue
mov  rbx, rax                  ; helper returns new stackTop
; total: ~25 bytes
```

### JIT compiler

```cpp
struct JitCode {
    uint8_t* code;      // mmap'd executable buffer
    size_t size;
    size_t capacity;
};

JitCode jitCompile(BblState& state, Chunk& chunk) {
    JitCode jit;
    jit.capacity = chunk.code.size() * 64;  // ~64 bytes per instruction avg
    jit.code = (uint8_t*)mmap(nullptr, jit.capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // Emit prologue: save callee-saved regs, set up rbx/r12/r13
    emitPrologue(jit);

    // Map bytecode offset → native code offset (for jump patching)
    std::vector<size_t> offsetMap(chunk.code.size(), 0);

    size_t ip = 0;
    while (ip < chunk.code.size()) {
        offsetMap[ip] = jit.size;
        uint8_t op = chunk.code[ip++];

        switch (op) {
        case OP_CONSTANT: {
            uint16_t idx = readU16(chunk, ip); ip += 2;
            BblValue* constPtr = &chunk.constants[idx];
            emitStencil(jit, stencil_constant, {(uintptr_t)constPtr});
            break;
        }
        case OP_ADD:
            emitStencil(jit, stencil_add, {(uintptr_t)&jitSlowAdd});
            break;
        case OP_GET_LOCAL: {
            uint16_t slot = readU16(chunk, ip); ip += 2;
            emitStencil(jit, stencil_get_local, {slot * sizeof(BblValue)});
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int16_t offset = (int16_t)readU16(chunk, ip); ip += 2;
            size_t target = ip + offset;
            // Record for later patching (target native offset not yet known)
            addJumpPatch(jit, jit.size, target);
            emitStencil(jit, stencil_jump_if_false, {0, 0}); // placeholders
            break;
        }
        // ... remaining opcodes ...
        }
    }

    // Second pass: patch all jump targets
    for (auto& patch : jumpPatches) {
        size_t nativeTarget = offsetMap[patch.bytecodeTarget];
        patchRel32(jit, patch.nativeOffset, nativeTarget);
    }

    // Emit epilogue
    emitEpilogue(jit);

    // Make executable
    mprotect(jit.code, jit.capacity, PROT_READ | PROT_EXEC);
    return jit;
}
```

### Helper functions

Stencils for complex operations (string concat, method calls, GC, etc.)
call C++ helper functions instead of inlining the logic.  These helpers
use the standard C ABI:

```cpp
// Called from JIT stencils via patched CALL instruction
extern "C" {
    BblValue* jitSlowAdd(BblState* state, BblValue* stackTop);
    BblValue* jitCallValue(BblState* state, BblValue* stackTop, int argc);
    BblValue* jitGetGlobal(BblState* state, uint32_t symId, BblValue* stackTop);
    void jitGcSafePoint(BblState* state);
    // ... etc
}
```

This keeps the stencils small and avoids duplicating complex logic (GC,
string interning, type coercion) in assembly.

### Execution model

```cpp
BblValue jitExecute(BblState& state, Chunk& chunk) {
    JitCode jit = jitCompile(state, chunk);

    // Call the JIT'd code as a function
    typedef BblValue (*JitFn)(BblState*, BblValue*, BblValue*);
    JitFn fn = (JitFn)jit.code;
    BblValue result = fn(&state, state.vm->stack.data(), state.vm->stack.data());

    munmap(jit.code, jit.capacity);
    return result;
}
```

### Scope

**Phase 1 — hot arithmetic path (~400 LOC):**
Stencils for: CONSTANT, NULL, TRUE, FALSE, POP, GET_LOCAL, SET_LOCAL,
ADD (int fast-path + slow call), SUB, MUL, DIV, MOD, LT, GT, LTE, GTE,
EQ, NEQ, NOT, JUMP, JUMP_IF_FALSE, LOOP, RETURN.

This covers the `loop_arith` benchmark.  Everything else falls back to
the interpreter.

**Phase 2 — function calls (~200 LOC):**
Stencils for: CALL, CLOSURE, GET_CAPTURE, SET_CAPTURE, GET_GLOBAL,
SET_GLOBAL, TAIL_CALL, AND, OR.

**Phase 3 — collections and methods (~200 LOC):**
Stencils that call C++ helpers for: VECTOR, TABLE, STRUCT, GET_FIELD,
SET_FIELD, GET_INDEX, SET_INDEX, METHOD_CALL, LENGTH, BINARY.

Total: ~800 LOC across `jit.h`, `jit.cpp`, `stencils_x86_64.h`.

---

## Expected Performance

The loop_arith hot loop currently executes 13 dispatches × ~2 cycles =
~26 cycles of dispatch overhead per iteration.  With copy-and-patch,
dispatch overhead drops to 0.  The actual work (2 adds, 1 compare,
loads/stores) is ~10-15 cycles.

| Benchmark | Current bytecode | After JIT (est.) | Lua 5.4 | LuaJIT |
|-----------|-----------------|-----------------|---------|--------|
| loop_arith (fn-wrapped) | 28 ms | ~5-8 ms | 12 ms | 2 ms |
| function_calls | 19 ms | ~8-12 ms | 16 ms | 1 ms |

On tight arithmetic loops, we should match or beat Lua 5.4.  Function
call-heavy code will still be slower than LuaJIT (which has optimized
call trampolines) but should match Lua 5.4.

---

## Risks

| Risk | Impact | Mitigation |
|------|--------|-----------|
| x86-64 only | Medium | Keep interpreter as fallback; JIT is opt-in |
| Security (W^X) | Low | mmap PROT_WRITE, mprotect PROT_EXEC after codegen |
| Stencil correctness | High | Test each stencil against interpreter output |
| Code size growth | Low | Stencils are ~30-60 bytes each; total < 3KB data |
| GC interaction | Medium | JIT'd code calls jitGcSafePoint at OP_LOOP |

---

## References

1. Xu, H. and Kjolstad, F. "Copy-and-Patch Compilation" (OOPSLA 2021).
   https://fredrikbk.com/publications/copy-and-patch.pdf

2. Xu, H. "Building a baseline JIT for Lua using Copy-and-Patch" (2023).
   https://sillycross.github.io/2023/05/12/2023-05-12/

3. CPython 3.13 JIT (PEP 744) — uses copy-and-patch for Python bytecode.
   https://docs.python.org/3.13/whatsnew/3.13.html

4. Brandner et al. "Copy-and-Patch compilation: a fast compilation
   algorithm for high-level languages and bytecode" — follow-up work.
