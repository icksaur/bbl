# Benchmark Results

Release build (-O2), GCC 15.2.1, Linux x86_64.
5 runs each, best-of-5 reported.

## Baseline (pre-optimization)

| benchmark | best time |
|-----------|-----------|
| loop_arith (1M iters) | 0.286s |
| function_calls (500K calls) | 0.220s |
| gc_pressure (100K allocs) | 0.030s |
| string_intern (10K concats) | 0.058s |
| table_heavy (1K entries) | 0.003s |

## After all optimizations

| benchmark | best time |
|-----------|-----------|
| loop_arith | 0.105s |
| function_calls | 0.078s |
| gc_pressure | 0.014s |
| string_intern | 0.061s |
| table_heavy | 0.002s |

## Comparison

| benchmark | baseline | optimized | speedup | target | status |
|-----------|----------|-----------|---------|--------|--------|
| loop_arith | 0.286s | 0.105s | **2.7x** | < 0.15s | PASSED |
| function_calls | 0.220s | 0.078s | **2.8x** | < 0.12s | PASSED |
| gc_pressure | 0.030s | 0.014s | **2.1x** | < 0.020s | PASSED |
| string_intern | 0.058s | 0.061s | ~1.0x | — | — |
| table_heavy | 0.003s | 0.002s | ~1.5x | — | — |

### Optimizations applied

1. **GC sweep partition** — replaced `vector::erase()` loops with `std::partition` + tail delete
2. **Adaptive GC threshold** — `gcThreshold = max(256, liveCount * 2)` after sweep
3. **Special-form dispatch table** — static `unordered_map` + `switch` replaces 15-branch `if` chain
4. **Parse-time string intern** — lazy-cached `BblString*` on `AstNode` skips repeat `intern()` calls
5. **Symbol IDs** — `uint32_t` keys in scope bindings replace `std::string` hashing
6. **Flat scope for function calls** — `vector<BblValue>` slots replace `unordered_map` for fn call scopes

---

## Round 2

### Bug fix

- **GC flat-scope slot marking** — `gcMarkScope()` now also marks `scope.slots`, fixing a use-after-free risk

### After Round 2 optimizations

| benchmark | best time |
|-----------|-----------|
| loop_arith | 0.078s |
| function_calls | 0.054s |
| gc_pressure | 0.012s |
| string_intern | 0.063s |
| table_heavy | 0.002s |

### Comparison (Round 1 → Round 2)

| benchmark | round 1 | round 2 | speedup | vs baseline |
|-----------|---------|---------|---------|-------------|
| loop_arith | 0.105s | 0.078s | **1.35x** | **3.7x** |
| function_calls | 0.078s | 0.054s | **1.44x** | **4.1x** |
| gc_pressure | 0.014s | 0.012s | **1.17x** | **2.5x** |
| string_intern | 0.061s | 0.063s | ~1.0x | ~1.0x |
| table_heavy | 0.002s | 0.002s | ~1.0x | ~1.5x |

### Optimizations applied (Round 2)

7. **Cached SpecialForm on AstNode** — first `lookupSpecialForm()` result cached as `int8_t` on the AST node, avoiding repeated hash lookups
8. **Stack-buffer for fn call args** — ≤8 args use a stack-local `BblValue[8]` instead of heap `vector`
9. **Direct param slot indexing** — `paramSlotStart` offset replaces `unordered_map::at()` lookup for parameter binding in `callFn`

---

## Round 3

### After Round 3 optimizations

| benchmark | best time |
|-----------|-----------|
| loop_arith | 0.065s |
| function_calls | 0.048s |
| gc_pressure | 0.010s |
| string_intern | 0.057s |
| table_heavy | 0.002s |

### Comparison (Round 2 → Round 3)

| benchmark | round 2 | round 3 | speedup | vs baseline |
|-----------|---------|---------|---------|-------------|
| loop_arith | 0.078s | 0.065s | **1.20x** | **4.4x** |
| function_calls | 0.054s | 0.048s | **1.13x** | **4.6x** |
| gc_pressure | 0.012s | 0.010s | **1.20x** | **3.0x** |
| string_intern | 0.063s | 0.057s | ~1.1x | ~1.0x |
| table_heavy | 0.002s | 0.002s | ~1.0x | ~1.5x |

### Optimizations applied (Round 3)

10. **Cached symbolId on set/def targets** — `resolveSymbol()` string hash cached via `mutable symbolId` on AstNode, same pattern as Symbol eval path
11. **Integer fast-path for comparisons** — `<`/`>`/`<=`/`>=` skip int→double conversion when both operands are `Int`
12. **Lazy bindings map initialization** — `BblScope::bindings` changed to `unique_ptr`, allocated only on first use; eliminates empty `unordered_map` heap allocation for every flat-mode function call scope
