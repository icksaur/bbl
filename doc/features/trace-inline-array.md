# Inline Array Get/Set in Trace Compiler
Status: proposed

## Goal

Reduce table_heavy from 5ms to ≤3ms by emitting inline array access code in `compileTrace` for OP_MCALL "get"/"set", matching the per-function JIT's inline array fast path.

## Background

The per-function JIT (`jitCompile`) already emits inline array fast paths for OP_MCALL "get" and "set": TAG_INT check on key → bounds check against `asize` → direct array load/store. Falls to C helper on miss. This was implemented in prior work and handles 100k integer-key get/set with zero C helper calls.

However, `compileTrace` emits a generic `jitMcall` C helper call for ALL OP_MCALL instructions. When the trace JIT activates for the table_heavy loops, it eliminates dispatch overhead but every table operation still round-trips through C (~20 cycles each). For 200k operations, that's ~4M cycles (~2ms at 2GHz).

## Design

In `compileTrace`'s OP_MCALL case, check if `methodStr` matches `state.m.get` or `state.m.set`. If so, emit the inline array fast path directly in the trace code instead of calling jitMcall.

For `get`: check TAG_INT on key, extract int, check `0 ≤ k < asize`, load `array[k]`, NaN-box as result. Fall to `jitTableGet` C helper on miss.

For `set`: same key check, store value at `array[k]`, increment count if empty slot. Fall to `jitTableSet` on miss.

The offsets `tblArrayOff`, `tblAsizeOff`, `tblCountOff` need to be available in `compileTrace`. Compute them the same way as in `jitCompile` (via dummy BblTable pointer arithmetic).

The receiver register (regs[A]) holds the NaN-boxed table value. Extract the table pointer with shl16/shr16 (TAG_OBJECT), then access the array fields.

## Considerations

- The trace compiler uses `entry.regBase` to offset register indices. All register references (A, B, C) must add `entry.regBase`.
- OP_MCALL's B operand is argc (number of args), and C is the constant index for the method string. The args start at regs[A+1].
- The `get` method reads key from `regs[A+1]`, stores result into `regs[A]`.
- The `set` method reads key from `regs[A+1]`, value from `regs[A+2]`, stores null into `regs[A]`.
- No need to handle `order` invalidation in the inline path — `BblTable::set()` in the C helper already handles that on the slow path.

## Acceptance

- table_heavy benchmark ≤ 3ms (currently 5ms)
- All 769 unit tests pass
- All 35 functional tests pass
- No regression on other benchmarks
