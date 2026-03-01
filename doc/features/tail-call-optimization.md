# Tail Call Optimization
Status: proposed

## Goal

Eliminate stack growth for self-recursive functions in tail position, enabling recursive algorithms to run in constant stack space.  Today, `(= f (fn (n acc) (if (== n 0) acc (f (- n 1) (+ acc n))))) (f 100000 0)` blows the call stack at 512 frames (`maxCallDepth`).  After TCO, it completes in O(1) stack space.

## Background

### Current call mechanics

`callFn` is the only entry point for BBL function calls.  It:

1. Checks `activeScopes.size() >= maxCallDepth` (stack overflow guard).
2. Creates a `BblScope callScope` with flat slots for captures + params.
3. Pushes `&callScope` onto `activeScopes`.
4. Iterates the function body: `for (auto& node : fn->body) { result = eval(node, callScope); ... }`.
5. Pops `activeScopes`, returns `result`.

Each recursive call nests another `callFn` frame on the C++ stack.  The `maxCallDepth` limit prevents actual stack overflow, but caps recursion to 512 frames.

### Tail positions in BBL

A call is in tail position if it is the last expression evaluated before the function returns.  In BBL's AST-interpreter model, tail positions are:

- **Last expression in a function body**: The final node in `fn->body`.
- **Then/else branches of `if`**: `(if cond (f x) (g x))` — both `(f x)` and `(g x)` are tail calls when `if` is itself in tail position.
- **Last expression in `do`**: `(do expr1 expr2 (f x))` — `(f x)` is a tail call when `do` is itself in tail position.
- **Last expression in `try` body**: Only if the try is in tail position.

Calls inside `loop`, `each`, `and`, `or`, `with`, and non-tail positions of `do` are never tail calls.

### Scope of this feature

**Self-recursion only.**  TCO for mutual recursion (A calls B calls A) requires a trampoline visible across callFn invocations, which is substantially more complex.  Self-recursion covers the primary use case: recursive traversals, folds, and accumulators.  The mechanism is a `goto`-based loop inside `callFn` — the simplest and most efficient approach for self-tail-calls.

## Design

### Detection: static tail-position marking

When a function is bound to a name (in the `SpecialForm::Eq` handler), mark AST nodes in the function body that are self-tail-calls.  This is a one-time walk of the function body, not a per-eval check.

A new field on `AstNode`:

```cpp
mutable bool isTailCall = false;
```

The marking algorithm `markTailCalls(fn->body, fnName)`:

1. If the body is empty, return.
2. Take the **last** node in the body.
3. If it is a `List` whose head is a `Symbol` matching `fnName`: set `node.isTailCall = true`.
4. If it is a `List` whose head resolves to `SpecialForm::If`: recurse into the then-branch (child 2) and else-branch (child 3, if present) — those become the "last expression" for their respective paths.
5. If it is a `List` whose head resolves to `SpecialForm::Do`: recurse into the last child of the do-block.
6. If it is a `List` whose head resolves to `SpecialForm::Try`: recurse into the last expression before the catch clause (child `size - 2`).  The catch handler body is not currently marked as a tail position — it could be in the future, but the primary gain is in the main body.
7. Otherwise: not a tail call.

This requires knowing the function's name.  The name is available when the function is defined via `(= name (fn ...))`.  For anonymous functions or functions assigned through other means, no tail calls are marked — this is correct and conservative.  The `SpecialForm::Eq` handler already has access to the symbol name and the fn body.

### Execution: goto loop in callFn

Wrap the body of `callFn` in a labeled loop.  When `eval` encounters a node with `isTailCall = true`, instead of recursing into `callFn`, it:

1. Evaluates the arguments.
2. Rebinds the parameters in-place in the existing `callScope.slots`.
3. Signals a restart via a sentinel mechanism.

The sentinel approach: a new exception-like struct `TailCall` that carries the evaluated arguments:

```cpp
struct TailCall {
    BblValue args[8];
    std::vector<BblValue> heapArgs;
    size_t argc;
};
```

This struct must be defined before `evalList` (which throws it), not before `callFn` (which catches it).  Place it after `lookupSpecialForm` around line ~983.

In `evalList`, when processing a function call where `node.isTailCall` is true and the resolved function is the same `BblFn*` currently executing:

1. Evaluate all arguments.
2. Throw `TailCall{args, argc}`.

In `callFn`, wrap the body loop in `while (true)`:

```cpp
BblValue result = BblValue::makeNull();
while (true) {
    try {
        for (auto& node : fn->body) {
            result = eval(node, callScope);
            checkTerminated();
            checkStepLimit();
            if (flowSignal) break;
        }
        break;  // normal return
    } catch (TailCall& tc) {
        // Arity check — must match param count
        if (tc.argc != fn->params.size()) {
            throw BBL::Error{"arity mismatch: expected " + std::to_string(fn->params.size())
                             + " argument(s), got " + std::to_string(tc.argc)};
        }
        // Rebind params in-place
        for (size_t i = 0; i < tc.argc; i++) {
            callScope.slots[fn->paramSlotStart + i] = tc.argc <= 8 ? tc.args[i] : tc.heapArgs[i];
        }
        // Re-load captures (they may have been mutated by the body)
        for (auto& [id, val] : fn->captures) {
            callScope.slots[fn->slotIndex.at(id)] = val;
        }
        // Loop continues — no new stack frame
    }
}
```

This reuses the existing scope, skips the `maxCallDepth` check on restart, and does not grow `activeScopes`.

### Alternative considered: continuation flag instead of exception

Instead of throwing `TailCall`, set a flag on `BblState` and return early from eval.  This would avoid the exception overhead but requires checking the flag at every return site in `evalList` and `eval`.  The exception approach is cleaner because `TailCall` unwinds naturally through `if`/`do`/`try` without touching their code.  In practice, the cost of a C++ throw/catch is negligible compared to interpreter overhead per expression.

### Name resolution for tail-call marking

The `SpecialForm::Eq` handler sees `(= name expr)`.  If `expr` evaluates to a function (after eval), the function is already built — its body AST is set.  The marking pass runs post-creation:

```cpp
case SpecialForm::Eq: {
    // ... existing code ...
    BblValue val = eval(node.children[2], scope);
    if (val.type == BBL::Type::Fn && !val.isCFn) {
        markTailCalls(val.fnVal->body, node.children[1].stringVal);
    }
    scope.def(nameId, val);
    // ...
}
```

This means tail calls are only marked for the **first** name a function is bound to.  `(= f (fn ...)) (= g f)` — `f` gets TCO, `g` calling itself as `g` does not (the AST was marked for `f`).  This is acceptable.

### Verifying the callee is the same function

The `isTailCall` flag marks the AST node, but at eval time we must verify the call target is actually the same `BblFn*` currently executing.  If the symbol has been rebound to a different function, the tail call must proceed as a normal call.

`callFn` does not currently know which `BblFn*` it's executing "by name."  But it receives the `BblFn* fn` pointer.  In `evalList`, when `isTailCall` is true:

1. Resolve the head symbol to get the `BblValue`.
2. If it's a Fn (not CFn) and `fnVal == currentlyExecutingFn` → tail call.
3. Otherwise → normal call.

To make the "currently executing fn" available, add a field to `BblState`:

```cpp
BblFn* currentFn = nullptr;
```

Set it at the top of `callFn`, restore it on exit.  `evalList` checks `node.isTailCall && headVal.fnVal == currentFn`.

## Considerations

### Interaction with checkTerminated / checkStepLimit

The `while(true)` loop in `callFn` re-executes the body, which hits `checkTerminated()` and `checkStepLimit()` on every iteration of every expression.  TCO does not bypass these checks.  An infinite tail-recursive loop with `maxSteps` set will still terminate.

### Interaction with backtraces

With TCO, tail-recursive calls reuse the same `callFn` C++ stack frame — the backtrace shows only one level of recursion for the recursive function.  This is standard behavior for TCO'd languages (Scheme, Erlang).  Note: `callFn` does not push to the `callStack` backtrace vector for BBL functions (only C-function calls do), so the backtrace is unaffected.

### Captures and closures

When a function captures variables from its enclosing scope, those captures are loaded into `callScope.slots` at call entry.  On tail-call restart, captures must be reloaded because the body may have mutated local bindings that shadow captures.  The reload is cheap — it's a small memcpy for each capture.

### maxCallDepth interaction

The `maxCallDepth` check happens once at function entry, not on tail-call restart.  This is correct — the stack doesn't grow on restarts.  Without TCO, a function that recurses 100000 times hits `maxCallDepth` at 512.  With TCO, it completes.

### C function calls are never tail-optimized

Only BBL `fn` calls can be tail-optimized.  C function calls (`isCFn = true`) go through a different dispatch path and are not candidates.

### `with` blocks are not tail-position

`with` has a destructor that must run before the function returns.  A call in the last expression of a `with` block is not in tail position because the destructor cleanup must happen after the call returns.

### Performance impact on non-tail calls

Zero.  The `isTailCall` flag defaults to `false`.  The `while(true)` loop in `callFn` breaks on the first iteration for non-recursive functions.  The catch clause is never entered.  No overhead.

### Anonymous functions

`(fn (x) (... (fn-self ...)))` — anonymous functions cannot be self-recursive by name (they have no name to call).  No TCO applies.  If a future `recur` keyword is added, it could explicitly signal self-tail-call without needing a name.

## Acceptance

- `(= f (fn (n acc) (if (== n 0) acc (f (- n 1) (+ acc n))))) (f 100000 0)` returns 5000050000 without stack overflow
- `maxCallDepth = 512` (default) — tail-recursive functions exceed 512 logical calls
- `maxSteps` still limits tail-recursive functions (counter increments on each body iteration)
- Non-tail recursive calls still hit `maxCallDepth` as before
- `checkTerminated()` still works for child-state teardown of tail-recursive functions
- Backtraces for tail-recursive functions show a single frame (not N frames)
- All existing tests pass unchanged
- Mutual recursion is NOT optimized (explicit non-goal)
- Anonymous functions are NOT optimized (no name to match)
