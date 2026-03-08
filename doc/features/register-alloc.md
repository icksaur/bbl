# Register Allocation for Arithmetic Loops
Status: proposed

## Goal

Reduce loop_arith from ~2× to ≤1.3× of LuaJIT by keeping loop variables in CPU registers across iterations, eliminating memory round-trips.

## Background

### Current bottleneck

Every arithmetic operation loads operands from the VM register file (`regs[A]` at `[rbx + A*8]`) and stores results back. Even tag-preserving ADD (the fastest path) does: `load [rbx+B*8] → rax; add rax, [rbx+C*8]; sub rax, TAG_INT; store [rbx+A*8]` — 2 memory reads + 1 write per ADD.

For the loop_arith hot path (10M iterations of `sum += i; i += 1`):
```
OP_LTJMP i, n       → 2 loads + compare
OP_ADD sum, sum, i   → 2 loads + 1 store + tag arithmetic
OP_ADDI i, 1         → 1 read-modify-write
OP_LOOP              → backward jump
```
Total: ~5 memory operations per iteration. L1 cache handles these fast, but they still create pipeline stalls from load-use latency (~4 cycles per load on modern x86).

### What LuaJIT does

LuaJIT's tracing JIT unboxes loop variables into native CPU registers and operates on raw integers. The loop body becomes pure register-to-register operations with zero memory traffic.

### Available CPU registers

BBL's JIT convention:
- **Reserved**: rbx (regs), r12 (state), r13 (chunk), r14 (call marker), rsp (stack)
- **Temporaries**: rax, rcx (used by current emit functions)
- **Free**: rdx, rsi, rdi, r8, r9, r10, r11 (7 registers)

In loops with no C helper calls, all 7 free registers are available for pinning VM registers.

## Design

### Approach: Unboxed loop variables

For eligible arithmetic loops, unbox VM registers into CPU registers at loop entry, operate on raw integers during the loop body, and rebox at loop exit. During the loop, operations are single x86-64 instructions with no memory access.

Example transformation for loop_arith:

**Before** (current, NaN-boxed in memory):
```asm
loop_start:
  cmp [rbx+i*8], [rbx+n*8]  ; LTJMP
  jge exit
  mov rax, [rbx+sum*8]      ; ADD
  add rax, [rbx+i*8]
  sub rax, TAG_INT
  mov [rbx+sum*8], rax
  add qword [rbx+i*8], 1    ; ADDI
  jmp loop_start             ; LOOP
exit:
```

**After** (unboxed in registers):
```asm
; prologue: unbox VM regs
mov r8, [rbx+n*8]; shl r8,16; sar r8,16    ; n → r8
mov r9, [rbx+sum*8]; shl r9,16; sar r9,16  ; sum → r9
mov r10, [rbx+i*8]; shl r10,16; sar r10,16 ; i → r10

loop_start:
  cmp r10, r8                ; LTJMP (pure register)
  jge epilogue
  add r9, r10                ; ADD (1 instruction!)
  inc r10                    ; ADDI (1 instruction!)
  jmp loop_start             ; LOOP
epilogue:
; rebox and store back
```

5 instructions per iteration vs ~11, zero memory operations.

### Phase 1: Pre-scan loop identification

Before the main JIT compilation pass, scan the bytecode array for OP_LOOP instructions. For each OP_LOOP at instruction index `loopEnd`:

1. Compute backward jump target: `loopStart = loopEnd + 1 + sBx` (sBx is negative)
2. Scan instructions `[loopStart, loopEnd]` for eligibility
3. If eligible, collect VM registers used and compute assignments

Store results in: `struct LoopAlloc { size_t startIdx, endIdx, exitIdx; int8_t regMap[256]; };`

Where `exitIdx` is `loopStart + 1` (the JMP after the LTJMP that exits the loop).

### Phase 2: Eligibility check

A loop is eligible if ALL of the following:
1. **All-arithmetic**: every instruction in the loop body is one of: LOADINT, ADD, SUB, MUL, ADDI, SUBI, ADDK, MOVE, LTJMP, LEJMP, GTJMP, GEJMP, JMP, JMPFALSE, JMPTRUE, LOOP, EQ, NEQ, LT, GT, LTE, GTE, LOADBOOL, LOADNULL, NOP
2. **No calls or heap ops**: no CALL, MCALL, TABLE, CLOSURE, GETGLOBAL, SETGLOBAL, GETFIELD, SETFIELD, etc.
3. **≤7 unique VM registers** used (we have 7 available CPU registers: rdx, rsi, rdi, r8-r11)
4. **All used VM registers are known-int** at the loop start point (from `knownTypes` tracking)
5. **No nested eligible loops** (only innermost loops for simplicity)

### Phase 3: Register assignment

Map VM registers to CPU registers in order of first use:

| Slot | CPU Register | x86-64 encoding |
|------|-------------|-----------------|
| 0 | r8 | REX.R bit |
| 1 | r9 | REX.R bit |
| 2 | r10 | REX.R bit |
| 3 | r11 | REX.R bit |
| 4 | rdx | normal |
| 5 | rsi | normal |
| 6 | rdi | normal |

Store the mapping in `regMap[vmReg] = hwSlot` (-1 for unmapped registers).

### Phase 4: Code generation

During the main JIT pass, when processing instructions inside a register-allocated loop:

**At loop start (instruction index == loopStart)**:
Emit prologue — for each mapped VM register, emit unbox + load:
```asm
mov hw_reg, [rbx + vm_reg * 8]  ; load NaN-boxed value
shl hw_reg, 16                   ; shift out tag
sar hw_reg, 16                   ; sign-extend payload
```

**During loop body**:
For each opcode, check if all operand VM registers are mapped. If yes, emit register-only instructions. The key emit functions needed:

- `emitAddHw(dst, src1, src2)`: `mov dst, src1; add dst, src2` (or just `add dst, src2` if dst == src1)
- `emitSubHw(dst, src1, src2)`: `mov dst, src1; sub dst, src2`
- `emitMulHw(dst, src1, src2)`: `mov rax, src1; imul rax, src2; mov dst, rax`
- `emitAddiHw(dst, imm)`: `add dst, imm32`
- `emitSubiHw(dst, imm)`: `sub dst, imm32`
- `emitCmpJmpHw(src1, src2, cc)`: `cmp src1, src2; jcc rel32`
- `emitLoadIntHw(dst, value)`: `mov dst, value` (raw int, not NaN-boxed)
- `emitMoveHw(dst, src)`: `mov dst, src`

If any operand is NOT mapped (shouldn't happen in eligible loops), fall back to normal code generation.

**At exit path (instruction index == exitIdx)**:
Emit epilogue before the exit JMP — for each mapped VM register, emit rebox + store:
```asm
mov rax, hw_reg
movabs rcx, PAYLOAD_MASK
and rax, rcx
movabs rcx, TAG_INT
or rax, rcx
mov [rbx + vm_reg * 8], rax
```

### Hardware register encoding

For x86-64 ModR/M encoding with extended registers (r8-r11):

| Register | REX prefix | ModR/M bits |
|----------|-----------|-------------|
| r8 | REX.B (0x41) or REX.R (0x44) | 000 |
| r9 | REX.B or REX.R | 001 |
| r10 | REX.B or REX.R | 010 |
| r11 | REX.B or REX.R | 011 |
| rdx | none | 010 |
| rsi | none | 110 |
| rdi | none | 111 |

The emit functions need to handle REX prefix construction for extended registers. Helper:
```cpp
struct HwReg {
    uint8_t code;    // 3-bit register code
    bool extended;   // needs REX.R or REX.B
};
static constexpr HwReg hwRegs[] = {
    {0, true},  // r8
    {1, true},  // r9
    {2, true},  // r10
    {3, true},  // r11
    {2, false}, // rdx
    {6, false}, // rsi
    {7, false}, // rdi
};
```

### What's NOT included

- No nested loop support (only innermost loops)
- No float register allocation (only int loops)
- No overflow detection (same limitation as existing knownTypes optimization)
- No spilling (if > 7 registers needed, loop is not eligible)
- No live-range splitting

## Considerations

### Integer overflow

Unboxed loop variables can exceed the 48-bit NaN-boxed payload range without detection. This is the same pre-existing limitation as `knownTypes`-based tag-preserving arithmetic. A future fix would add `jo` (jump on overflow) after arithmetic, but this is out of scope.

### Non-integer loop variables

If a loop uses any non-integer variables (floats, strings, tables), the entire loop is ineligible. This is conservative but safe. Future work could support mixed-type loops by only pinning integer registers.

### LOADBOOL / LOADNULL inside loops

These produce non-integer values. If they target a mapped register, the loop should be ineligible. In practice, LOADBOOL is often used for conditional assignments which don't appear in tight arithmetic loops.

### Interaction with existing optimizations

- **knownTypes**: Still tracks types normally. The register allocator only activates for loops where all registers are already known-int.
- **Tag-preserving arithmetic**: Not used inside register-allocated loops (unboxed values have no tags). Used normally outside loops.
- **Inline table caches**: Not applicable (eligible loops have no OP_MCALL).
- **Trace JIT**: The selective trace hook at OP_LOOP checks for TABLE presence. Register-allocated loops won't have TABLE, so the trace hook won't fire.

### Code size

Each eligible loop adds: prologue (~20 bytes per register) + epilogue (~24 bytes per register). For 3 registers: ~132 bytes overhead. The loop body itself shrinks (fewer bytes per instruction). Net impact is small.

### Correctness of rebox at epilogue

The epilogue reboxes all mapped registers, including those that might not have been modified inside the loop. This is harmless — we're just storing back the same value. It ensures the VM register file is consistent after the loop.

## Acceptance

- loop_arith benchmark ≤ 1.3× of LuaJIT (currently ~2.0×)
- All 769 unit tests pass
- All 35 functional tests pass
- No regression on other benchmarks
- Only innermost all-integer loops are affected; all other code paths unchanged
