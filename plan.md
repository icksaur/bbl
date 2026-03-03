# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

## legend

[ ] incomplete
[x] complete

---

## overview

Tracing JIT per `doc/features/tracing-jit.md`.  Record hot loop
execution paths, compile to native code via existing stencils.

Build: `cmake --build build`
Test: `./build/bbl_tests`

---

## phase 1 — trace recorder and compiler for single-function traces

[ ] Add `uint16_t hotCount = 0`, `void* traceCode = nullptr`,
    `bool traceCompiled = false` to Chunk in chunk.h.

[ ] Add trace structures to jit.h: TraceEntry, Trace, SideExit.
    Declare recordTrace() and compileTrace().

[ ] Implement recordTrace() in jit.cpp: walk bytecode from loop body
    start, record each instruction. Stop at LOOP. Abort on unsupported ops.

[ ] Implement compileTrace() in jit.cpp: emit stencils linearly from
    recorded trace. LTJMP → guard + side exit. LOOP → backward jmp.

[ ] Integrate into VM OP_LOOP: hot counter, record+compile at threshold,
    execute trace when compiled.

[ ] Build, test (780 pass), benchmark loop_arith.

## phase 2 — call inlining in traces

[ ] Extend recordTrace(): when CALL hit, follow into callee bytecode
    with regBase adjustment. When RETURN hit, pop regBase and continue.
    Limit depth to 4.

[ ] Extend compileTrace(): adjust register offsets by regBase per entry.
    Emit type guard at inlined CALL site.

[ ] Build, test, benchmark recursion (fib 35). Target: < 100ms.

## phase 3 — trace caching

[ ] Cache compiled trace on Chunk. Clear on dealloc.
    Run full benchmark suite. No regressions.
