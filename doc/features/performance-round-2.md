# Performance Round 2
Status: done

## Goal

Fix a correctness bug introduced during round 1 (GC doesn't mark flat-scope slots) and squeeze further gains from four low-risk, low-complexity optimizations targeting the same three benchmarks.

| benchmark | current | target | what limits it now |
|-----------|---------|--------|--------------------|
| loop_arith | 0.105 s | < 0.085 s | `lookupSpecialForm` hash per eval, `resolveSymbol` hash per symbol |
| function_calls | 0.078 s | < 0.060 s | `vector<BblValue>` heap alloc per call, `slotIndex.at()` hash per binding |
| gc_pressure | 0.014 s | — | already 2.1× target; no further changes planned |

## Design

### 0. Bug fix: GC must mark flat-scope slots

**Problem.** `gcMarkScope()` iterates only `scope.bindings`, but flat-mode function call scopes store captures and params exclusively in `scope.slots`. If a GC fires during a call body, live GC-managed values in slots (fns, tables, structs, vectors, binaries, userdata) won't be marked — use-after-free.

**Fix.** In `gcMarkScope()`, also iterate `scope.slots` and call `gcMark()` on each.

### 1. Cache SpecialForm on AstNode

**Problem.** Every `evalList` call invokes `lookupSpecialForm(op)`, which hashes a `std::string` against a 24-entry `unordered_map`. In `loop_arith`, the loop body evaluates `<`, `+`, `set` millions of times — each re-hashing the same operator string.

**Fix.** Add `mutable int8_t cachedSpecialForm = -1` to `AstNode`. On first `evalList` entry for a List node whose head is a Symbol, compute the SpecialForm and cache it. Subsequent visits read the cached int directly — no hash, no string compare.

### 2. Small-buffer args for function calls

**Problem.** Every user-function call constructs `std::vector<BblValue> args` on the heap. Most functions have 0-4 parameters. The heap alloc/free pair (~50-100 ns) is a significant fraction of the ~156 ns/call.

**Fix.** Use a stack-local `BblValue buf[8]` and pass a pointer+count to `callFn`. Change `callFn` signature to `(BblFn*, const BblValue*, size_t, int)`. The C-function path must use `callArgs.assign(args, args + argc)` instead of move. If arity > 8, fall back to heap vector. This avoids the heap round-trip for the common case. The `function_calls` benchmark does 500K calls, so the current ~156 ns/call budget makes the alloc/free pair significant.

### 3. Direct slot indices in callFn

**Problem.** `callFn` calls `fn->slotIndex.at(id)` for each capture and each param to find slot positions. This is an `unordered_map<uint32_t, size_t>` lookup per binding — O(1) amortized but still involves hashing and a branch.

**Fix.** Store `size_t paramSlotStart` on `BblFn`. Captures occupy slots `[0, captures.size())`, params occupy `[paramSlotStart, paramSlotStart + params.size())`. At call time, copy captures by flat index and params by `paramSlotStart + i` — zero hash lookups.

The `slotIndex` map is still needed for `scope.lookup/set/def` during body evaluation, so it stays.

## Considerations

- **symbolId caching already in place.** When `evalList` enters the function-call path and calls `eval(head, scope)`, the Symbol eval case already lazy-caches `symbolId` on the AstNode. No additional work needed.

- **Item 0** is a correctness fix. It must land first. All existing tests pass today only because no benchmark passes GC-managed values through function args in a long-lived loop. A targeted test should be added.
- **Item 2** changes the `callFn` signature from `const std::vector<BblValue>&` to `const BblValue*, size_t`. Internal-only — no public API impact.
- **Item 3** requires that slot layout is captures-first, params-second — which is already the case from round 1.
- No BBL syntax or semantics changes. No public API changes.

## Acceptance

- GC bug fixed with a targeted test proving it.
- All 306 unit + 17 functional tests pass.
- loop_arith, function_calls benchmarks show measurable improvement.
- Results appended to `benchmarks.md`.
