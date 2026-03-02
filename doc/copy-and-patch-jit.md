# Copy-and-Patch JIT for BBL
Status: proposed

## Goal

Compile bytecode to native x86-64 machine code at runtime by copying
pre-compiled instruction templates (stencils) and patching in concrete
register offsets and constants.  Eliminates all dispatch overhead.

Expected: match LuaJIT interpreter speed on arithmetic loops (~2-3ms
for loop_arith vs current 6ms).

---

## BblValue layout

Verified via offsetof:

    offset 0: type     (BBL::Type, 4 bytes)
    offset 4: isCFn    (bool, 1 byte)
    offset 5: isClosure (bool, 1 byte)
    offset 8: intVal / floatVal / stringVal / etc (union, 8 bytes)
    sizeof(BblValue) = 16

All stencils use these offsets.  Register R[i] is at byte offset
i * 16 from the register base pointer.

---

## Design

### Register convention

JIT'd code uses callee-saved registers (safe across C helper calls):

    rbx = BblValue* regs       (frame register file base)
    r12 = BblState* state      (for C helper calls)
    r13 = Chunk* chunk         (for constant pool access)

### Stencil format

```cpp
struct Stencil {
    const uint8_t* code;
    uint8_t size;
    struct Patch { uint8_t offset; uint8_t type; };
    const Patch* patches;
    uint8_t patchCount;
};

enum PatchType : uint8_t {
    PATCH_DISP32,   // 32-bit displacement (register byte offset)
    PATCH_IMM32,    // 32-bit immediate value
    PATCH_REL32,    // 32-bit relative jump/call target
    PATCH_ABS64,    // 64-bit absolute address
};
```

### Stencil examples

Notation: A_OFF = A*16, INTVAL = +8, TYPE = +0.

**ADD R[A] = R[B] + R[C] (int fast-path):**
```x86
mov  rax, [rbx + B_OFF+8]       ; 48 8b 83 <B_OFF+8:disp32>
add  rax, [rbx + C_OFF+8]       ; 48 03 83 <C_OFF+8:disp32>
mov  [rbx + A_OFF+8], rax       ; 48 89 83 <A_OFF+8:disp32>
mov  dword [rbx + A_OFF], 2     ; c7 83 <A_OFF:disp32> 02 00 00 00
```
25 bytes, 3 displacement patches.

**ADDI R[A] += sBx (int in-place):**
```x86
add  qword [rbx + A_OFF+8], imm ; 48 81 83 <A_OFF+8:disp32> <imm:imm32>
```
11 bytes, 1 displacement + 1 immediate patch.

**LTJMP — if R[A] < R[B], skip next stencil (continue loop):**
The VM semantics: if (cond) skip next instruction.  In JIT'd code,
"skip next instruction" means jump past the next stencil.

```x86
mov  rax, [rbx + A_OFF+8]       ; load R[A].intVal
cmp  rax, [rbx + B_OFF+8]       ; compare to R[B].intVal
jl   <skip:rel32>                ; if A < B (cond TRUE), skip next stencil
```
20 bytes.  The jl target is patched to the byte address after the
next stencil (which is the JMP-to-exit).  If A >= B (cond FALSE),
fall through to the JMP which exits the loop.

**LOOP — backward jump + GC safe point:**
```x86
jmp  <loop_start:rel32>          ; e9 <rel32>
```
5 bytes.  GC check: every 256th iteration, insert a call to
jitGcSafePoint before the jmp.

**LOADINT R[A] = sBx:**
```x86
mov  qword [rbx + A_OFF+8], imm ; 48 c7 83 <A_OFF+8:disp32> <imm:imm32>
mov  dword [rbx + A_OFF], 2     ; c7 83 <A_OFF:disp32> 02 00 00 00
```
17 bytes, sets type=Int and intVal=imm.

**CALL — delegate to C helper:**
```x86
mov  rdi, r12                    ; state pointer
lea  rsi, [rbx + A_OFF]         ; &R[A]
mov  edx, argc                  ; argument count
movabs rax, <&jitCallHelper>    ; 64-bit address
call rax
```
~25 bytes.  C helper handles all call mechanics.

### JIT compilation

```cpp
JitCode jitCompile(BblState& state, Chunk& chunk) {
    // Allocate writable buffer
    size_t capacity = chunk.code.size() * 32 + 256;
    uint8_t* buf = (uint8_t*)mmap(NULL, capacity,
        PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    size_t pos = 0;

    // Prologue: push callee-saved regs, set rbx/r12/r13
    // ... ~20 bytes ...

    // Map bytecode instruction index → native code offset
    std::vector<size_t> nativeOffsets(chunk.code.size() + 1);

    for (size_t i = 0; i < chunk.code.size(); i++) {
        nativeOffsets[i] = pos;
        uint32_t inst = chunk.code[i];
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst), B = decodeB(inst), C = decodeC(inst);
        int sBx = decodesBx(inst);

        switch (op) {
        case OP_ADD:
            emitStencil(buf, pos, stencil_add, B*16+8, C*16+8, A*16+8, A*16);
            break;
        case OP_ADDI:
            emitStencil(buf, pos, stencil_addi, A*16+8, sBx);
            break;
        case OP_LTJMP:
            emitStencil(buf, pos, stencil_ltjmp, A*16+8, B*16+8);
            // Patch jl target after emitting next stencil
            recordForwardPatch(i, pos - 4);  // rel32 is last 4 bytes
            break;
        case OP_JMP:
            emitStencil(buf, pos, stencil_jmp);
            recordJumpPatch(i, pos - 4, i + 1 + sBx);
            break;
        case OP_LOOP:
            emitStencil(buf, pos, stencil_jmp);  // reuse jmp stencil
            // Patch to nativeOffsets[i + 1 - sBx] (backward)
            patchRel32(buf, pos - 4, nativeOffsets[i + 1 - sBx]);
            break;
        case OP_RETURN:
            emitStencil(buf, pos, stencil_return, A*16+8);
            break;
        // ... remaining opcodes ...
        default:
            // Fallback: call interpreter for unsupported opcodes
            emitInterpreterFallback(buf, pos, i);
            break;
        }
    }
    nativeOffsets[chunk.code.size()] = pos;

    // Patch all forward jumps
    for (auto& p : forwardPatches)
        patchRel32(buf, p.patchOffset, nativeOffsets[p.targetInst]);

    // Epilogue + make executable
    // ... ~10 bytes ...
    mprotect(buf, pos, PROT_READ|PROT_EXEC);
    return { buf, pos, capacity };
}
```

### Execution

```cpp
BblValue jitExecute(BblState& state, Chunk& chunk) {
    JitCode jit = jitCompile(state, chunk);
    typedef BblValue (*JitFn)(BblValue* regs, BblState* state, Chunk* chunk);
    JitFn fn = (JitFn)jit.buf;
    BblValue result = fn(state.vm->stack.data(), &state, &chunk);
    munmap(jit.buf, jit.capacity);
    return result;
}
```

### Scope

**Phase 1 — hot arithmetic path (~300 LOC):**
Stencils: LOADK, LOADINT, LOADNULL, LOADBOOL, ADD, ADDI, SUB, SUBI,
MUL, DIV, MOD, LTJMP, LEJMP, GTJMP, GEJMP, JMP, LOOP, MOVE, RETURN.
Also ADDK (constant pool add).

Fallback to interpreter for: CALL, CLOSURE, MCALL, GETGLOBAL,
SETGLOBAL, GETCAPTURE, SETCAPTURE, VECTOR, TABLE, GETFIELD, SETFIELD,
GETINDEX, SETINDEX, TRYBEGIN, TRYEND, EXEC, EXECFILE, AND, OR,
JMPFALSE, JMPTRUE, NOT, LT, GT, LTE, GTE, EQ, NEQ.

This covers the loop_arith hot loop (LTJMP, ADD, ADDI, LOOP = 4
stencils per iteration, ~62 bytes of native code, zero dispatch).

**Phase 2 — globals and calls (~200 LOC):**
Add GETGLOBAL, SETGLOBAL, CALL, RETURN (with frame), GETCAPTURE,
SETCAPTURE, CLOSURE.  These delegate to C helpers via stencils.

**Phase 3 — full coverage (~200 LOC):**
Remaining opcodes via C helper stencils.

### Type guards

Arithmetic stencils assume integer operands.  For correctness, each
stencil includes a type check with a slow-path call:

```x86
cmp  dword [rbx + B_OFF], 2     ; check R[B].type == Int
jne  slow
cmp  dword [rbx + C_OFF], 2     ; check R[C].type == Int
jne  slow
; ... fast int path ...
jmp  next
slow:
mov  rdi, r12                   ; call C helper for general case
; ...
next:
```

Adds ~15 bytes per arithmetic stencil.  Branch predictor learns
that integers are the common case after 1-2 iterations.

---

## Expected performance

loop_arith hot loop: 4 bytecode instructions, each ~11-25 bytes of
native code, no dispatch = ~62 bytes total per iteration.  At ~15-20
native instructions with zero dispatch overhead, expect ~2-3ms.

| Benchmark | Interpreter | JIT (est.) | LuaJIT |
|-----------|-------------|-----------|--------|
| loop_arith | 6 ms | ~2-3 ms | 5 ms |

---

## Risks

| Risk | Mitigation |
|------|-----------|
| x86-64 only | Interpreter fallback; --no-jit flag |
| W^X security | mmap WRITE, mprotect EXEC after codegen |
| Stencil encoding bugs | Test each stencil against interpreter |
| GC during JIT code | Safe points via counter + C helper call |
| Code cache pressure | JIT code ~200 bytes per function |

---

## References

1. Xu & Kjolstad, "Copy-and-Patch Compilation" (OOPSLA 2021)
2. CPython 3.13 JIT (PEP 744)
3. Xu, "Building a baseline JIT for Lua" (2023)
