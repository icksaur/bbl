# Zero-Based Table Push
Status: done

## Goal

Change `table.push` from 1-based to 0-based auto-incrementing integer keys.  Currently the first `push` assigns key 1, the second key 2, etc.  After this change the first `push` assigns key 0, the second key 1, etc.  This aligns `push` with `at` (already 0-based) and with standard programming language conventions.

## Motivation

- `at` is already 0-based: `(t.at 0)` returns the first int-keyed entry.
- `push` starting at 1 creates an off-by-one mismatch: after `(t.push "x")`, `(t.at 0)` returns `"x"` but `(t.get 0)` returns `null` and `(t.get 1)` returns `"x"`.
- Standard Lisp (Common Lisp, Scheme) uses 0-based indexing for arrays/lists.
- Lua-style 1-based was the original design but it causes confusion and AI benchmark models frequently stumble on the mismatch.

## Design

### Core change

In `bbl.h`, change `BblTable::nextIntKey` initializer from `1` to `0`:

```cpp
int64_t nextIntKey = 0;  // was 1
```

That's the only C++ code change needed.  The `set` method already tracks `nextIntKey = key.intVal + 1` for any explicit integer key, so manual `(t.set 0 val)` still works correctly.

### keys method

The `keys` method (which returns a table of all keys) currently assigns 1-based integer keys to its result table.  This must also change to 0-based to be self-consistent: the returned table's push behavior should match.

In `bbl.cpp`, change `int64_t idx = 1;` to `int64_t idx = 0;` in the `keys` handler.

### No change needed

- `at` — already 0-based positional access, unaffected.
- `pop` — removes highest integer key, unaffected.
- `set` — explicit key, unaffected.
- `get` — explicit key, unaffected.
- `table` constructor — explicit key-value pairs, unaffected.

## Impact

### Tests
- `test_table_push_pop` — no assertion change needed (tests length and pop value, not key values)
- `test_table_method_first_resolution` — `mlen` checks `(t.length)` on a table with 1 explicit entry = 1.  Unaffected.
- `test_table_closure_shared_capture` — checks `(t.length)` = 1 after push. Unaffected.

### bblbench reference scripts
- `3_sort.bbl` — uses 1-based `(data.get 1)` through `(data.get N)` for sorting.  Must change to 0-based: `(data.get 0)` through `(data.get (- N 1))`.
- `5_closure.bbl` — uses `at` for iteration (already 0-based).  Unaffected.
- `1_file_gen.bbl`, `2_primes.bbl`, `4_collatz.bbl` — don't use push-based integer access.  Unaffected.

### bblbench expected outputs
- All expected `.stdout` files remain the same (logic outputs don't change, just internal key numbering).

### Documentation
- `doc/bbl.md` — update push description (remove "1-based" references if any, clarify 0-based).
- `doc/features/table.md` — update push description.
- `bbl-bench.md` and `bbl-bench/bench.sh` — update sort task prompt (change "1-based keys" to "0-based keys").
- `bbl-bench/doc/bbl.md` — mirror of doc/bbl.md, update accordingly.

## Considerations

- This is a breaking change for any existing BBL scripts that use `push` then `get` with explicit integer keys starting from 1.  However, BBL is pre-release and the only consumers are the bblbench reference scripts.
- The `at` method is already 0-based, so this resolves the mismatch rather than creating one.
- Lua uses 1-based; BBL is intentionally diverging from Lua on this point.

## Acceptance

1. `nextIntKey` initializes to 0 in `bbl.h`.
2. `keys` method uses 0-based output keys.
3. All 328+ unit tests pass.
4. All 18 functional tests pass.
5. All 5 bblbench scripts pass (`run.sh` checksum verification).
6. Docs updated consistently.
7. Bench prompts updated to say "0-based keys".
