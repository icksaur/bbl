# Performance Improvements
Status: proposed

## Goal

Reduce per-iteration and per-call overhead in the three hottest benchmarks without changing BBL semantics or the tree-walking architecture.

| benchmark | baseline | target | what limits it |
|-----------|----------|--------|----------------|
| loop_arith (1M iters) | 0.281 s (~280 ns/iter) | < 0.15 s | eval dispatch, scope lookup |
| function_calls (500K calls) | 0.216 s (~430 ns/call) | < 0.12 s | scope creation, capture copying |
| gc_pressure (100K allocs) | 0.031 s | < 0.020 s | sweep cost |

These are the workloads that matter for the game-scripting use case: tight update loops, many small function calls, bursty allocation.

## Design

Six improvements, ordered by expected impact on the hot benchmarks.

### 1. Special-form dispatch table

**Problem.** `evalList` tests the operator string against ~15 sequential top-level `if (op == "...")` branches (bbl.cpp `evalList()`). Each branch may contain sub-checks (e.g., arithmetic dispatches `+`/`-`/`*`/`/`/`%` inside one outer `if`). Arithmetic operators sit ~13 comparisons deep. Every list evaluation pays the full linear scan until a match.

**Fix.** Build a `static const std::unordered_map<std::string, SpecialFormHandler>` at startup (or use a `constexpr` perfect hash). Look up the operator in O(1) and dispatch through a function pointer or `std::function`. Fall through to user-function call on miss.

**Impact.** Eliminates linear string comparison overhead on every expression. Biggest win for loop_arith where `+`, `<`, `set`/`=` are evaluated millions of times.

### 2. Flat scope for function calls

**Problem.** `callFn` (bbl.cpp line 1510+) creates a fresh `BblScope` with an `std::unordered_map<std::string, BblValue>` on every call. For a closure with 2 captures + 2 args, this constructs a 4-entry hash map, copies captures in, binds args, evaluates, then destructs the map. Hash map construction/destruction dominates the ~430 ns per call.

**Fix.** Replace `BblScope` for function-call frames with a flat binding array:
- At `fn` creation time, compute a slot index for each parameter and capture name. Store the slot layout on `BblFn`.
- At call time, allocate a `std::vector<BblValue>` (or small-buffer-optimized fixed array) of the right size, copy captures and args by index, and use it as the frame.
- `lookup()` on a flat frame is a direct index. Function call scopes have no parent (closures see only captures + args), so a miss is an undefined-symbol error — no fallback needed.

**Impact.** Eliminates hash map construction per call. Biggest win for function_calls benchmark.

### 3. GC sweep: partition instead of erase

**Problem.** GC sweep (bbl.cpp line 582+) iterates each of 6 allocation pool vectors and calls `vector::erase()` on dead entries. Erase from the middle of a vector is O(n) (shifts trailing elements). In the worst case the sweep is O(n²) per pool.

**Fix.** Use `std::partition` (or a manual swap-and-pop loop) to move all live objects to the front, then `delete` the dead tail and `resize`. This makes sweep O(n) per pool.

Alternative: use a single `std::vector<GcObject*>` with a type tag, so there is one pool to sweep instead of six. This also simplifies adding new GC-managed types.

**Impact.** Reduces gc_pressure benchmark time. Also reduces pause spikes in allocation-heavy scripts.

### 4. Adaptive GC threshold

**Problem.** `gcThreshold` is a fixed constant (256). After each GC cycle, `allocCount` resets to 0 and the next GC triggers after 256 more allocations regardless of how many objects are live. In steady state with many live objects, GC runs too often; in scripts with few objects, 256 is fine.

**Fix.** After each sweep, set `gcThreshold = max(256, liveCount * 2)`. This is the standard heuristic (Lua, Go, etc.): the next GC triggers when total allocations have doubled relative to the surviving set. The multiplier (2) can be a tunable constant.

**Impact.** Fewer GC cycles in allocation-heavy scripts. Reduces gc_pressure time and avoids unnecessary sweep work when most objects are live.

### 5. Intern string literals at parse time

**Problem.** Every evaluation of a `StringLiteral` AST node calls `intern(node.stringVal)`, which hashes and looks up the string in `internTable`. In a loop body with string literals, this repeats the same hash on every iteration.

**Fix.** Intern during parsing: when the parser creates a `StringLiteral` node, call `intern()` once and store the resulting `BblString*` pointer on the AST node. At eval time, return the cached pointer directly.

This requires the parser to have access to the `BblState` (it already does for `parse()`), and the AST node to carry an optional `BblString*` field.

**Impact.** Eliminates repeated string hashing in loops. Minor win for loop_arith (no strings), moderate win for scripts with string literals in hot paths.

### 6. Symbol ID for scope lookup

**Problem.** `scope.lookup(name)` takes a `const std::string&` and calls `unordered_map::find()` at each scope level, hashing the string each time. In a deep scope chain, the same symbol name is hashed repeatedly.

**Fix.** Assign each unique symbol name a numeric ID at parse time (via a symbol table on `BblState`). Store the ID on the AST `Symbol` node. Change `BblScope::bindings` to use the integer ID as key (or keep `unordered_map<uint32_t, BblValue>` — integer hashing is a single instruction). Lookup becomes integer comparison instead of string hash + compare.

This pairs well with improvement 2 (flat scope): function-frame lookup becomes `frame[slotIndex]`, and only the fallback to parent scopes needs the ID-based map.

**Impact.** Reduces scope lookup cost. Combined with flat scopes, most lookups become O(1) array index. Biggest win in loop_arith where `sum`, `i`, `n` are looked up millions of times.

## Considerations

**Backwards compatibility.** All six improvements are internal to the interpreter. No BBL syntax or semantics change. The C++ embedding API contract is preserved — all public methods keep their string-based signatures. Internally, string-keyed API methods (`def`, `set`, `get`, `has`, `defn`, etc.) route through `resolveSymbol()` to convert names to IDs.

**Complexity budget.** Items 1, 3, and 4 are simple refactors (< 50 lines each). Items 2, 5, and 6 touch the parser and scope system and are more invasive (~100-200 lines each). They should be implemented and tested individually to avoid destabilization.

**Rejected alternatives.**
- *Bytecode VM:* 5-20× improvement for compute loops, but a fundamentally different architecture. Out of scope for this round — documented in performance.md.
- *NaN-boxing BblValue:* Would shrink BblValue from 24 bytes to 8 bytes and improve cache density. High complexity and risk of subtle bit-manipulation bugs. Defer.
- *Arena allocator for scopes:* Would speed up allocation but complicates lifetime management. The flat-scope approach (item 2) gets most of the benefit without arena complexity.
- *String rope / builder:* Would fix O(n²) concat, but string concat in tight loops is rare in the game-scripting use case. Defer.

**Risk.** The flat-scope and symbol-ID changes (items 2 and 6) interact with each other and with closure capture. They must be implemented in sequence (6 before 2) with thorough test coverage after each step. Item 5 (parse-time string interning) also modifies `AstNode` and should be coordinated with item 6. The symbol-ID change (item 6) also affects every public API method that takes a string name — all must be updated to resolve names to IDs internally.

**Measurement.** Each improvement must be validated against the three benchmarks before and after. Use `hyperfine --warmup 3` for stable timing. If an improvement does not measurably help, revert it.

## Acceptance

- All six improvements implemented and merged.
- All existing tests pass (306 unit + 17 functional).
- Benchmark results recorded in `doc/performance.md` showing measurable improvement:
  - loop_arith < 0.15 s (was 0.281 s)
  - function_calls < 0.12 s (was 0.216 s)
  - gc_pressure < 0.020 s (was 0.031 s)
- No new public API surface. No BBL syntax changes.
