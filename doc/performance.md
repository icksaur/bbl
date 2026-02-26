# Performance

This document has our performance strategy, classification of performance issues, profiling strategy, and common classes of performance bottlenecks in BBL.

## testing strategy

### benchmarks

All benchmarks live in `tests/bench/` and are `.bbl` scripts run via `time`. They fall into three categories:

| benchmark | measures |
|-----------|----------|
| `loop_arith.bbl` | tight arithmetic loop — eval/scope lookup overhead |
| `table_heavy.bbl` | table insert/lookup at scale — O(n) entry scan |
| `function_calls.bbl` | deep call chains — scope creation, closure capture |
| `gc_pressure.bbl` | rapid allocation — GC trigger frequency & sweep cost |
| `string_intern.bbl` | string creation at scale — intern table pressure |

### profiling

1. Build with `-O2 -g` (CMake RelWithDebInfo).
2. Run with `perf record ./build/bbl tests/bench/<name>.bbl`.
3. Analyze with `perf report` — focus on top-5 hotspots.
4. Compare before/after optimization with `hyperfine` or wall-clock `time`.

## known performance characteristics

### AST tree-walking interpreter

BBL evaluates by walking the AST directly.  No bytecode compilation.  This means:
- Every expression evaluates through `eval()` → `switch` on node type.
- Function calls re-evaluate the body AST each time.
- This is the dominant cost in compute-heavy scripts.

**Classification: architectural.** A bytecode VM would be 5-20× faster but is out of scope for the current design.  This is documented, not fixed.

### scope lookup

Variable lookup walks the parent-pointer chain calling `std::unordered_map::find()` at each level.  For deeply nested scopes (e.g., closures inside loops), this is O(depth × hash).

**Classification: moderate.** Most scripts have shallow scopes (3-5 levels).  Not worth optimizing unless profiling shows it as a bottleneck.

### table operations — O(n) linear scan

`BblTable` stores entries as `std::vector<pair<BblValue, BblValue>>`.  `get`, `set`, `has`, `del` all do linear scans.  This is fine for small tables (< 50 entries) — the typical use case — but degrades for large tables.

**Classification: design choice.** Tables are ordered and support mixed key types, which makes a hash map impractical without a custom hasher.  For large collections, vectors with integer indexing (`at`) are O(1).

### GC sweep cost

GC mark-and-sweep walks all allocation pools.  Sweep cost is proportional to total live objects, not just garbage.  With a low threshold (256), GC runs frequently in allocation-heavy code.

**Classification: tunable.** Increase `gcThreshold` for allocation-heavy workloads.  A generational GC would help but is out of scope.

### string interning

Every string literal and string operation hits `internTable` (an `unordered_map`).  This is O(1) amortized but has constant-factor overhead from hashing + comparison.  Interned strings are never freed, so memory grows monotonically for string-heavy scripts.

**Classification: design choice.** Interning guarantees O(1) string equality (pointer compare) and deduplication.  The monotonic growth is acceptable for typical script lifetimes.

## optimization targets (by impact)

1. **Reduce eval dispatch overhead** — e.g., inline common paths, avoid `std::string` copies in symbol lookup.
2. **Reduce allocation/GC frequency** — pool small objects, bump-allocate.
3. **Table lookup for large tables** — add hash index as optimization layer.
4. **Scope lookup** — flatten scope chain for hot paths.

## results

Results recorded with Release build (`-O2`), GCC 15.2.1, Linux x86_64.

### baseline

| benchmark | time | notes |
|-----------|------|-------|
| `loop_arith` (1M iterations) | 0.281s | eval dispatch + scope lookup + int arithmetic |
| `table_heavy` (1K entries) | 0.002s | negligible at this scale |
| `function_calls` (500K calls) | 0.216s | scope creation + arg binding overhead |
| `gc_pressure` (100K table allocs) | 0.031s | GC sweeping 100K tables |
| `string_intern` (10K concats) | 0.076s | O(n²) string growth pattern (sys time from allocation) |

### analysis

- **eval dispatch** is the primary bottleneck for compute-heavy scripts (~280ns/iteration for simple arithmetic). This is inherent to tree-walking interpretation.
- **function call overhead** is ~430ns/call (slightly slower per-iteration due to scope setup + arg binding).
- **GC** is well-behaved: 100K allocations with periodic collection complete in 31ms.
- **string concat** shows expected O(n²) behavior from repeated concatenation of growing strings. The sys time (48ms) suggests malloc/memcpy dominance. This is a known pattern — scripts should avoid `(+ s "x")` in tight loops.
- **table operations** are fast for small tables (1K entries, 2ms total for insert + read-all).

### GC safety fix

Fixed a critical bug: GC was triggered from allocation functions, which could run mid-expression-evaluation before new values were rooted in any scope. This caused segfaults in allocation-heavy loops. Fix: GC now triggers only at safe points (top of `exec`/`execExpr` statements, loop iterations) where all live values are in scope bindings. Active function call scopes are also tracked and marked.

### optimization opportunities (not pursued — documented for future work)

1. **Bytecode compilation** would eliminate tree-walking overhead (est. 5-20× improvement for compute loops). Out of scope.
2. **String builder / rope** would fix O(n²) concat pattern. Low priority — uncommon in game scripting use case.
3. **Table hash index** would improve lookup for large tables. Low priority — tables are typically small.