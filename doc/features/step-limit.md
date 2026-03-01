# Step Limit
Status: proposed

## Goal

Provide a configurable execution step limit so C++ API users can prevent runaway scripts from hanging their process.  The CLI can be interrupted with ctrl+c, but embedders have no equivalent — a bad script with `(loop true null)` blocks the calling thread forever.

## Background

### Existing limits

`maxCallDepth` (default 512) limits function call nesting.  It checks `activeScopes.size()` at the top of `callFn` and throws `BBL::Error{"stack overflow: ..."}`.  This only catches runaway recursion — an infinite `(loop true null)` with no function calls never grows the scope stack and hangs forever.

### Existing checkpoint sites

The child-states feature added `checkTerminated()` calls at the 5 eval-loop checkpoint sites — the same locations where control flow must pass through on every iteration or expression:

1. `do` block — after each expression
2. `loop` body — after each iteration
3. `each` body — after each iteration
4. `try` body — after each expression
5. `callFn` body — after each expression in function body

These sites guarantee that any loop, function body, or `do` block checks at least once per expression.  The step limit reuses these same sites.

## Design

### New field: `maxSteps`

Add a public field to `BblState`:

```cpp
size_t maxSteps = 0;       // 0 = unlimited (default)
size_t stepCount = 0;      // current step counter
```

`maxSteps = 0` means no limit, matching the opt-in pattern of `maxCallDepth`.  Embedders set it before calling `exec()`/`execfile()`:

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.maxSteps = 1'000'000;  // limit to 1M steps
try {
    bbl.exec(userScript);
} catch (const BBL::Error& e) {
    // e.what == "step limit exceeded: 1000000 steps"
}
```

### Checkpoint increment

At each of the 5 existing `checkTerminated()` sites, add a step counter check immediately after:

```cpp
checkTerminated();
checkStepLimit();
```

Where `checkStepLimit()` is an inline method:

```cpp
void checkStepLimit() {
    if (maxSteps && ++stepCount > maxSteps)
        throw BBL::Error{"step limit exceeded: " + std::to_string(maxSteps) + " steps"};
}
```

The counter increments at the same rate as `checkTerminated()` — once per expression in `do`, `try`, and function bodies, once per iteration in `loop` and `each`.

### Error type: `BBL::Error`

The step limit throws `BBL::Error`, not `BblTerminated`.  Rationale:

- `BBL::Error` is catchable by both C++ API and script-level `try`/`catch`.  An embedder wrapping `exec()` in `try { ... } catch (BBL::Error& e)` will see the limit error.
- `BblTerminated` is reserved for parent-state teardown of child threads — an internal mechanism that must bypass user-level catch.
- This matches `maxCallDepth`, which also throws `BBL::Error`.

A script that catches `BBL::Error` inside a loop could theoretically suppress the step limit, but the counter keeps incrementing so the next checkpoint throws again — the script cannot escape.

### Counter does not auto-reset

`stepCount` persists across `exec()` calls on the same `BblState`.  This is intentional — multiple `exec()` calls should share a cumulative budget.  The embedder can reset manually:

```cpp
bbl.stepCount = 0;  // reset before next exec() if desired
```

### Child states

Child states are independent `BblState` instances on their own threads.  A child inherits nothing from the parent's step limit — the embedder controls the child's limits by setting fields before spawning, or via `addChildStates` defaults.  The default `maxSteps = 0` (unlimited) matches current behavior.

If an embedder wants bounded children, they can wrap `state-new` in a C++ function that sets the child's `maxSteps`.  This is consistent with how `maxCallDepth` works — it's a per-state field, not inherited.

## Considerations

### Why not a wall-clock timeout?

A step counter is deterministic — the same script always hits the limit at the same point.  Timeouts depend on CPU speed and system load, making behavior hard to reproduce.  Step counting is also cheaper (a single increment + compare vs. a clock read).  An embedder that wants wall-clock limits can use `terminated.store(true)` from another thread, which already works via `checkTerminated()`.

### Counter granularity

One step per checkpoint hit.  A tight `(loop true null)` increments once per iteration.  A function with 10 expressions in its body increments 10 times per call.  This is coarser than "one step per AST node" but matches the existing checkpoint granularity and avoids adding overhead to simple expressions like `(+ 1 2)`.

The `catch` handler body does not have a checkpoint (same as `checkTerminated`) — only the `try` body does.  This is acceptable because the catch body has a fixed number of AST expressions; it cannot loop independently.

### Overflow

`stepCount` is `size_t`.  At 1 billion steps per second, a 64-bit counter overflows in ~584 years.  No overflow protection needed.

### Performance

The check is a branch on a local field — no atomic, no memory fence.  When `maxSteps == 0` (the common case), the branch is trivially not-taken and predicted away.  Cost: unmeasurable.

## Acceptance

- `maxSteps = 0` (default): no limit, existing behavior unchanged
- `maxSteps = N`: throws `BBL::Error{"step limit exceeded: N steps"}` after N checkpoint hits
- Infinite `(loop true null)` terminates promptly when `maxSteps` is set
- Infinite recursion `(= f (fn () (f))) (f)` terminates via `maxCallDepth` (the `callFn` checkpoint fires after body expressions return, which never happens in unbounded recursion — `maxSteps` does not add recursion protection)
- `stepCount` persists across multiple `exec()` calls and can be reset manually
- Script-level `try`/`catch` catches the error but cannot escape (counter keeps going)
- All existing tests pass with `maxSteps = 0`
- Child states unaffected (their own `maxSteps` defaults to 0)
