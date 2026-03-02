# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

## legend

[ ] incomplete
[x] complete

---

## overview

Copy-and-patch JIT for BBL per `doc/copy-and-patch-jit.md`.  Compile
register bytecode to native x86-64 by copying stencil templates and
patching register offsets.  Phase 1 only: arithmetic hot path.

BblValue layout: type at +0 (4 bytes), intVal at +8 (8 bytes), sizeof=16.
Register R[i] is at byte offset i*16 from register base (rbx).

Build: `cmake --build build`
Test: `./build/bbl_tests`

---

## phase 1 — stencil infrastructure

[ ] Create `jit.h` in repo root:
    - `struct JitCode { uint8_t* buf; size_t size; size_t capacity; };`
    - `struct Stencil { const uint8_t* code; uint8_t size; ... };`
    - Declare `JitCode jitCompile(BblState& state, Chunk& chunk);`
    - Declare `void jitFree(JitCode& jit);`

[ ] Create `jit.cpp` in repo root:
    - `#include <sys/mman.h>` for mmap/mprotect.
    - Helper: `emitBytes(buf, pos, data, len)` — memcpy into buffer.
    - Helper: `patchRel32(buf, offset, target)` — write relative offset.
    - `jitCompile()`: allocate mmap buffer, emit prologue (push rbx/r12/r13,
      mov rbx=rdi, mov r12=rsi, mov r13=rdx), iterate instructions,
      emit stencils, patch jumps, emit epilogue (pop, ret), mprotect.
    - `jitFree()`: munmap.

[ ] Define stencils as constexpr byte arrays in `jit.cpp`:
    All offsets verified against BblValue layout (type=+0, intVal=+8).

    `stencil_add`: R[A] = R[B] + R[C] (int fast path, ~40 bytes with type guard):
      - cmp dword [rbx + B*16], 2 / jne slow
      - cmp dword [rbx + C*16], 2 / jne slow
      - mov rax, [rbx + B*16+8] / add rax, [rbx + C*16+8]
      - mov [rbx + A*16+8], rax / mov dword [rbx + A*16], 2
      - jmp next / slow: <call C helper> / next:
    Patches: 3 register displacements (A, B, C), 1 rel32 for slow path.

    `stencil_addi`: R[A].intVal += imm32 (11 bytes):
      - add qword [rbx + A*16+8], imm32
    Patches: 1 displacement (A), 1 imm32.

    `stencil_ltjmp`: if R[A] < R[B] skip next stencil (20 bytes):
      - mov rax, [rbx + A*16+8] / cmp rax, [rbx + B*16+8] / jl <skip:rel32>
    Patches: 2 displacements (A, B), 1 rel32 (skip target).
    NOTE: jl = "jump if less" = skip when condition TRUE (matching VM
    semantics where true condition skips the exit JMP).

    `stencil_jmp`: unconditional jump (5 bytes):
      - jmp <rel32>
    Patches: 1 rel32.

    `stencil_loop`: same as jmp (backward).

    `stencil_loadint`: set R[A] = imm32 int (17 bytes):
      - mov dword [rbx + A*16], 2 / mov qword [rbx + A*16+8], imm32
    Patches: 1 displacement (A), 1 imm32.

    `stencil_loadk`: R[A] = constants[Bx] (copy 16 bytes via movups):
      - movabs rsi, <&constants[Bx]> / movups xmm0, [rsi] / movups [rbx + A*16], xmm0
    Patches: 1 abs64 (constant address), 1 displacement (A).

    `stencil_move`: R[A] = R[B] (copy 16 bytes):
      - movups xmm0, [rbx + B*16] / movups [rbx + A*16], xmm0
    Patches: 2 displacements (A, B).

    `stencil_loadnull`: set R[A].type = 0, R[A].intVal = 0:
      - xorps xmm0, xmm0 / movups [rbx + A*16], xmm0
    Patches: 1 displacement (A).

    `stencil_return`: move R[A] to return register, restore, ret:
      - movups xmm0, [rbx + A*16] / movups [rsp+retval_offset], xmm0
      - pop r13 / pop r12 / pop rbx / ret
    Patches: 1 displacement (A).

    `stencil_sub`, `stencil_subi`, `stencil_mul`, `stencil_div`, `stencil_mod`:
    Same structure as add/addi.

    `stencil_addk`: R[A] = R[B] + constants[C] (int fast path).

    `stencil_lejmp`, `stencil_gtjmp`, `stencil_gejmp`: same as ltjmp
    with different condition code (jle, jg, jge).

    `stencil_loadbool`: set R[A].type = 1, R[A].boolVal.

    Unsupported opcodes: emit a call to `jitFallback(state, ip_index)`
    which runs the interpreter for that single instruction and returns.

[ ] Add to CMakeLists.txt: `jit.cpp` in bbl_lib sources.

[ ] Wire into execution: in `bbl.cpp`, when `useBytecode` is true and
    the chunk has no unsupported opcodes in the hot path, call
    `jitExecute()` instead of `vmExecute()`.  Add `--jit` flag to
    main.cpp (opt-in for now).

[ ] Build and verify: `cmake --build build`.

## phase 1 — testing

[ ] Add unit tests to `tests/test_bbl.cpp`:
    ```
    TEST(test_jit_arithmetic)    // (+ 1 2) == 3
    TEST(test_jit_loop)          // loop_arith == 499999500000
    TEST(test_jit_if)            // (if true 1 2) == 1
    TEST(test_jit_fn_call)       // (= f (fn (x) (+ x 1))) (f 10) == 11
    ```
    These use `--jit` mode via `bbl.useJit = true`.

[ ] Run all 780 existing tests (interpreter mode must still pass).

[ ] Build release and benchmark:
    ```
    cmake --build build_rel
    time ./build_rel/bbl --jit tests/bench/loop_arith.bbl
    time luajit tests/bench/loop_arith.lua
    ```
    Target: loop_arith < 4ms.
