# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

## legend

[ ] incomplete
[x] complete

---

## overview

Replace the stack-based bytecode VM with a register-based VM per
`doc/features/register-vm.md`.  Rewrite chunk.h, compiler.cpp, vm.cpp,
disasm.cpp.  Keep vm.h structures with minimal changes.  No parser changes.

Build: `cmake --build build`
Test: `./build/bbl_tests && ./tests/run_functional.sh ./build/bbl`
Benchmark: `time ./build_rel/bbl --bytecode tests/bench/loop_arith.bbl`

---

## phase 1 — instruction encoding and data structures

[ ] Rewrite `chunk.h`:
    - Change `OpCode` enum to register-format opcodes (LOADK, ADD, ADDK,
      SUB, MUL, DIV, MOD, LT, GT, LTE, GTE, EQ, NEQ, NOT, BAND, BOR,
      BXOR, BNOT, SHL, SHR, LOADNULL, LOADBOOL, LOADINT, JMP, JMPFALSE,
      JMPTRUE, LOOP, MOVE, GETGLOBAL, SETGLOBAL, GETCAPTURE, SETCAPTURE,
      CALL, TAILCALL, RETURN, CLOSURE, VECTOR, TABLE, STRUCT, BINARY,
      GETFIELD, SETFIELD, GETINDEX, SETINDEX, MCALL, LENGTH, SIZEOF,
      TRYBEGIN, TRYEND, EXEC, EXECFILE, AND, OR).
    - Change `Chunk::code` from `vector<uint8_t>` to `vector<uint32_t>`.
    - Add `uint8_t numRegs` field to `Chunk`.
    - Change `Chunk::lines` to one entry per instruction (not per byte).
    - Add helpers: `emitABC(op,A,B,C,line)`, `emitABx(op,A,Bx,line)`,
      `emitAsBx(op,A,sBx,line)`.  Each appends one uint32_t.
    - Keep `addConstant()`, `patchU16()` → `patchsBx(offset, sBx)`.

[ ] Update `vm.h`:
    - Change `CallFrame::ip` from `uint8_t*` to `uint32_t*`.
    - Rename `CallFrame::slots` to `CallFrame::regs`.
    - Add `uint8_t numRegs` to `CallFrame` (copied from chunk at call time).

[ ] Build: verify compilation of chunk.h and vm.h changes.

## phase 2 — compiler rewrite

[ ] Rewrite `compiler.h`:
    - Add `RegAllocator` struct: `nextReg`, `maxRegs`, `alloc()`, `free()`,
      `freeFrom()`.
    - Update `CompilerState`: replace `locals` vector with
      `std::unordered_map<uint32_t, uint8_t> localRegs` (symbol ID → register).
      Keep `enclosing`, `captures`, `loops`, `scopeDepth`, `fnName`, `arity`.
    - `compileNode` signature: `uint8_t compileExpr(state, cs, node, destReg)`
      returns the register holding the result (may differ from destReg if
      the value is already in a known register).

[ ] Rewrite `compiler.cpp` — literals and arithmetic:
    - `IntLiteral`: if small (-32768..32767), emit `LOADINT dest sBx`.
      Otherwise `LOADK dest Bx`.
    - `FloatLiteral`, `StringLiteral`, `BinaryLiteral`: `LOADK dest Bx`.
    - `BoolLiteral`: `LOADBOOL dest B`.
    - `NullLiteral`: `LOADNULL dest`.
    - `Symbol`: look up in `localRegs` → return that register (no instruction).
      If capture → `GETCAPTURE dest idx`.  If global → `GETGLOBAL dest Bx`.
    - Arithmetic (`+`,`-`,`*`,`/`,`%`): compile LHS into regB, RHS into regC.
      If RHS is a constant, use ADDK (for `+` only).  Emit `ADD dest B C`.
      For variadic `(+ a b c)`: `ADD t1 B C; ADD dest t1 D`.
    - Comparisons: `LT dest B C` etc.
    - Bitwise: `BAND dest B C` etc.  `BNOT dest B`.
    - `not`: `NOT dest B`.

[ ] Rewrite `compiler.cpp` — variables and control flow:
    - `=` (symbol target): compile value into a temp reg, then `MOVE localReg temp`
      (or just compile directly into localReg).  First `=` to new name allocates
      a register via `localRegs[symId] = regAlloc.alloc()`.
    - `=` (dot target): compile obj into regB, compile val into regA,
      `SETFIELD A B Bx`.
    - `=` (index target): `SETINDEX A B C`.
    - `if`: compile cond into regA, `JMPFALSE A offset`, compile then into dest,
      `JMP endOffset`, compile else into dest.
    - `loop`: mark start, compile cond into regA, `JMPFALSE A exitOffset`,
      compile body (result discarded), `LOOP backOffset`.  Emit `LOADNULL dest`
      after exit.
    - `break`: `JMP exitOffset` (patched).
    - `continue`: `LOOP backOffset`.
    - `do`: compile each child, last one goes into dest.
    - `and`: compile LHS into dest, `AND dest offset`, compile RHS into dest.
      (AND = "if falsy(R[A]), skip; else continue")
    - `or`: same pattern with `OR`.

[ ] Rewrite `compiler.cpp` — functions and closures:
    - `fn`: push new CompilerState.  R[0] = callee, R[1..arity] = params.
      Set `localRegs` for each param.  Compile body into a result reg.
      `RETURN resultReg`.  Store chunk as constant.  Emit `CLOSURE dest Bx`
      with capture descriptors (same format as current: srcType + srcIdx).
    - Self-capture for `(= name (fn ...))`: after CLOSURE, `SETCAPTURE A selfIdx`.
    - Tail calls: detect tail position, emit `TAILCALL A B` instead of `CALL`.
    - Regular call `(f arg1 arg2)`: put callee in R[base], args in
      R[base+1..base+argc].  `CALL base argc 1`.

[ ] Rewrite `compiler.cpp` — remaining forms:
    - `each`: allocate regs for container, length, index, element.
      Loop with GETINDEX, increment, LOOP.
    - `vector`: compile type name as constant.  Compile C elements into
      R[dest+1..dest+C].  Emit `VECTOR dest Bx C` (type name = constants[Bx]).
    - `table`: compile B key-value pairs into R[dest+1..dest+2B].
      Emit `TABLE dest B`.
    - `struct`: compile C field args into R[dest+1..dest+C].
      Emit `STRUCT dest Bx C` (type name = constants[Bx]).
    - `binary`, `sizeof`: same as current.
    - `try`: `TRYBEGIN A sBx`, compile body, `TRYEND`, compile catch.
    - DotAccess: `GETFIELD dest B Bx` or `GETINDEX dest B C`.
    - ColonAccess: compile receiver into R[dest], args into R[dest+1..dest+B].
      Emit `MCALL dest B Bx` (method name = constants[Bx]).
    - `exec`, `execfile`: compile arg, emit opcode.
    - Struct constructors: compile args into R[dest+1..dest+C].
      Emit `STRUCT dest Bx C`.

[ ] Rewrite top-level `compile()`: wrap in implicit function
    (numRegs = regAlloc.maxRegs).

## phase 3 — VM rewrite

[ ] Rewrite `vm.cpp` dispatch loop:
    - Read `uint32_t inst = *frame->ip++`.
    - Decode A, B, C, Bx, sBx from inst.
    - `#define R(i) frame->regs[i]`
    - `#define K(i) frame->chunk->constants[i]`
    - Implement all opcode handlers using register reads/writes.
    - Computed-goto dispatch table indexed by `op`.
    - GC safe point at LOOP (same as current).
    - Step limit / termination check at LOOP and CALL (same as current).

[ ] Rewrite `callValue()`:
    - For closures: push CallFrame, set `regs = stackTop`, advance
      stackTop by `closure->chunk.numRegs`.  Copy args from caller's
      registers into callee's R[1..argc].
    - For C functions: copy args from registers into `state.callArgs`,
      call cfn, put result in R[A].
    - For tree-walker BblFn: same as current (call `state.callFn`).

[ ] Rewrite RETURN handler:
    - Copy result from R[A] to caller's destination register.
    - Pop frame, restore `frame` pointer.
    - Reset stackTop to frame->regs + frame->numRegs.

[ ] Exception handling:
    - TRYBEGIN: push handler with frameIdx, catchIp, errorReg.
    - Catch block: on error, unwind to handler, put error string in R[A].
    - try/catch around dispatch loop (same pattern as current).

## phase 4 — disassembler and testing

[ ] Rewrite `disasm.cpp`:
    - Decode 32-bit instructions.
    - Print format: `0000  ADD      R3 R1 R2`.
    - Handle all instruction formats (iABC, iABx, iAsBx).

[ ] Run all 780 tests: `cmake --build build && ./build/bbl_tests`.
    Fix any failures.

[ ] Run functional tests: `./tests/run_functional.sh ./build/bbl`.

[ ] Build release and benchmark:
    ```
    cmake --build build_rel
    time ./build_rel/bbl --bytecode tests/bench/loop_arith.bbl
    time lua5.4 tests/bench/loop_arith.lua
    ```
    Target: loop_arith within 1.5x of Lua 5.4.