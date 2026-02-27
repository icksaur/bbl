# each — index-only iteration
Status: done

## Goal

Add an `each` special form that iterates over a table or vector, binding only the loop index variable. The user accesses elements via `(container.at i)` inside the body.  This eliminates the 3-line boilerplate (`= i 0`, `loop (< i len)`, `= i (+ i 1)`) that every counted iteration requires today.

## Design

### Syntax

```bbl
(each i container body...)
```

- `i` — a **symbol** (the loop variable name). Bound as an int starting at 0, incremented each iteration.
- `container` — an expression that evaluates (once) to a **table** or **vector**.
- `body...` — one or more body expressions evaluated each iteration.

### Semantics

1. Evaluate `container` **once** before the loop begins.
2. Compute `len` = `(container.length)`.
3. For `i` from 0 to `len - 1`: bind `i` in the enclosing scope, evaluate all body expressions.
4. After the loop, `i` remains bound (value = `len`). This matches `loop` behavior — `loop` doesn't create a scope either.
5. `each` is a statement — returns `null`.

### Equivalent desugaring

```bbl
(each idx data
    (print (data.at idx) "\n"))
```

is equivalent to:

```bbl
(= idx 0)
(= _len (data.length))
(loop (< idx _len)
    (print (data.at idx) "\n")
    (= idx (+ idx 1)))
```

### Why index-only (not value-binding)

BBL's value model splits types: value types (int, float, bool, struct) are copied on assignment, while GC types (table, vector, string, fn) are shared references. A value-binding `each` like `(each x container ...)` would silently copy ints but share tables — confusing semantics. Index-only avoids this: the user writes `(container.at i)` and the usual value-vs-reference rules apply transparently.

### Works on both tables and vectors

Both have `.length` and `.at` methods, so `each` works uniformly on both. For tables, `each` iterates over integer-keyed entries by 0-based position (same as `table.at`).

### Scope rules

`each` runs in the **enclosing scope** (like `loop`). The index variable is created via `=` semantics (assign-or-create). It does NOT create a child scope. This is consistent with all other BBL control flow forms.

### Interaction with closures

A `fn` defined inside `each`'s body captures the index variable by value (snapshot at capture time), following existing capture rules. Since `each` rebinds the index each iteration, each closure captures a different snapshot — the naturally correct behavior.

The `gatherFreeVars` function must treat `each` like `loop` but also mark the index variable as bound so it is not incorrectly listed as a free variable.

## Considerations

- **No break/return:** Like `loop`, `each` has no early exit. The `found`-flag pattern still applies. This is acceptable — `each` targets the common case.
- **Empty containers:** If `len` is 0, the body never executes. `i` is still bound (value 0).
- **Nested each:** Multiple `each` forms work fine since each binds a different index variable name.
- **GC pressure:** The container is evaluated once and held during iteration. If the container is a temporary, it stays alive.
- **Mutation during iteration:** Mutating the container inside `each` (push/pop) is undefined behavior, same as modifying while iterating with `loop`. No guard needed.

## Acceptance

1. `each` iterates over a vector, body can read elements via `.at`.
2. `each` iterates over a table, body can read elements via `.at`.
3. `each` with empty container executes zero iterations, index variable is bound to 0.
4. `each` with non-table/non-vector container throws a type error.
5. `each` with non-symbol index throws an error.
6. `each` with missing arguments throws an error.
7. Index variable is visible after `each` completes (value = length).
8. Closures defined inside `each` capture the correct index snapshot.
9. Nested `each` with different index variables works correctly.
10. `gatherFreeVars` correctly handles `each` — index var is not a false free variable.
11. All existing tests still pass (328+ unit, 5 bblbench).
12. `doc/bbl.md`, `bbl-bench/doc/bbl.md`, `doc/spec.md`, and `bbl-pain.md` updated.
13. bblbench scripts updated where `each` simplifies existing iteration patterns.
