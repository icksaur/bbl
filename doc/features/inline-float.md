# Inline Float Arithmetic for JIT
Status: proposed

## Goal

Add inline x86-64 float arithmetic (addsd/mulsd/divsd) to the per-function JIT, eliminating C helper calls for float operations. Target: spectral_norm 147ms → ≤20ms.

## Background

BBL's NaN-boxing stores floats as raw IEEE 754 doubles — no tag bits to strip. Loading a float from the register file into an XMM register is a single `movsd`. Despite this, ALL float arithmetic currently goes through the `jitArith` C helper (~20 cycles per call). The spectral_norm benchmark does ~5M float multiply-adds, each requiring 2 C helper calls = 10M calls.

## Design

### Part 1: Emit functions for float arithmetic

Add `emitFAdd`, `emitFSub`, `emitFMul`, `emitFDiv`:
```
movsd xmm0, [rbx + B*8]   ; load operand B (raw IEEE 754)
addsd xmm0, [rbx + C*8]   ; operate with operand C
movsd [rbx + A*8], xmm0   ; store result (raw IEEE 754)
```
No unbox/rebox needed. 3 instructions vs ~50 through C helper.

x86-64 SSE2 encoding:
- `movsd xmm0, [rbx+disp32]` = `F2 0F 10 83 <disp32>` (8 bytes)
- `addsd xmm0, [rbx+disp32]` = `F2 0F 58 83 <disp32>` (8 bytes)
- `mulsd xmm0, [rbx+disp32]` = `F2 0F 59 83 <disp32>` (8 bytes)
- `divsd xmm0, [rbx+disp32]` = `F2 0F 5E 83 <disp32>` (8 bytes)
- `movsd [rbx+disp32], xmm0` = `F2 0F 11 83 <disp32>` (8 bytes)

### Part 2: KnownType::Float propagation

Add `Float` to the KnownType enum. Propagation rules:
- OP_DIV result → Float (always)
- OP_LOADK with float constant → Float
- Float+Float ADD/SUB/MUL → Float (use emitFAdd/FMul)
- Float+Int or Int+Float → Float (convert int operand with cvtsi2sd, then float op)
- MOVE propagates source type

When both operands are KnownType::Float, emit inline float ops directly — skip the type guard entirely.

### Part 3: OP_ADD/SUB/MUL dispatch

In the OP_ADD handler, add a new branch before the existing int fast path:
```cpp
if (knownTypes[B] == KnownType::Float && knownTypes[C] == KnownType::Float) {
    emitFAdd(jit.buf, jit.size, A, B, C);
    knownTypes[A] = KnownType::Float;
    break;
}
```

For mixed int+float, emit int-to-float conversion then float op.

### Part 4: OP_DIV inline

Replace `emitCallHelper2(jitArith, ...)` for OP_DIV with inline `emitFDiv`. Mark result as KnownType::Float.

## Considerations

- XMM registers are caller-saved in System V ABI — no need to save/restore across C calls
- Float comparison (for loop bounds with float) not needed for spectral_norm since loop counters are ints
- The register allocator (XMM allocation for float loops) is a future step — this change only does memory-based float ops

## Acceptance

- spectral_norm produces correct result (42888.6...)
- spectral_norm ≤ 30ms (currently 147ms)
- All 769 unit tests pass
- All 35 functional tests pass
- No regression on integer benchmarks
