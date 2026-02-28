# Backlog Batch: string ordering, break/continue, vector resize/reserve, try/catch
Status: implemented

## Goal

Implement four remaining backlog items in one batch.  Each is small-to-trivial in scope and touches non-overlapping code.

---

## 1. String Ordering

### Design

Add string comparison to the existing `CmpLt`/`CmpGt`/`CmpLe`/`CmpGe` case block (bbl.cpp ~line 1447).  Before the numeric type guard, insert a string-string branch that uses `std::string::compare` (byte-level, deterministic, no locale).

```cpp
if (left.type == BBL::Type::String && right.type == BBL::Type::String) {
    int cmp = left.stringVal->data.compare(right.stringVal->data);
    switch (sf) {
        case SpecialForm::CmpLt: return BblValue::makeBool(cmp < 0);
        case SpecialForm::CmpGt: return BblValue::makeBool(cmp > 0);
        case SpecialForm::CmpLe: return BblValue::makeBool(cmp <= 0);
        case SpecialForm::CmpGe: return BblValue::makeBool(cmp >= 0);
        default: break;
    }
}
```

Cross-type comparison (string vs int) remains a type error.

### Acceptance

1. `(< "a" "b")` → `true`
2. `(> "z" "a")` → `true`
3. `(<= "abc" "abc")` → `true`
4. `(>= "abc" "abd")` → `false`
5. `(< "A" "a")` → `true` (uppercase < lowercase in ASCII)
6. `(< "a" 1)` → type error (cross-type)

---

## 2. Break / Continue

### Design

Use a state flag on `BblState` rather than sentinel exceptions (exceptions are expensive on the hot path — break/continue fire every iteration in tight loops).

Add `uint8_t flowSignal = 0` to `BblState` with constants `FlowNone = 0`, `FlowBreak = 1`, `FlowContinue = 2`.

New special forms `Break` and `Continue`:

```cpp
case SpecialForm::Break:    flowSignal = FlowBreak; return BblValue::makeNull();
case SpecialForm::Continue:  flowSignal = FlowContinue; return BblValue::makeNull();
```

Flow signal checks in body-evaluating constructs:

- **Loop** body (bbl.cpp ~line 1141): after each `eval` in the body for-loop, check `if (flowSignal) break;`.  After the body loop, check: if `FlowBreak` → reset signal, break the while loop.  If `FlowContinue` → reset signal, continue the while loop.
- **Each** body (bbl.cpp ~line 1178): same pattern — check after each inner eval; after body loop, break vs continue.
- **Do** body (bbl.cpp ~line 1108): after each `eval`, `if (flowSignal) break;` — do NOT reset the signal (let it propagate to enclosing loop).
- **If** branches (bbl.cpp ~line 1121-1124): propagation only — if the branch eval sets `flowSignal`, it propagates naturally since `if` returns immediately.

Stray break/continue outside a loop: `exec()` checks `flowSignal` after the eval loop and throws `BBL::Error{"break outside of loop"}` / `BBL::Error{"continue outside of loop"}`.

### Considerations

- `break` and `continue` take no arguments.  `(break 42)` is a parse-time arity error.
- Inside `with`, break/continue propagate correctly: the body eval sets `flowSignal`, the body for-loop breaks, cleanup runs, then the signal propagates up to the enclosing loop.
- Inside `fn`, break/continue should NOT propagate across function boundaries.  `callFn` must reset `flowSignal` if set, and throw `BBL::Error{"break outside of loop"}`.  This prevents a `break` inside a callback from silently exiting the caller's loop.

### Acceptance

1. `(loop true (break))` — exits immediately, no infinite loop.
2. `(= sum 0) (= i 0) (loop (< i 10) (if (== i 5) (break)) (= sum (+ sum i)) (= i (+ i 1)))` — sum = 10 (0+1+2+3+4).
3. `(= sum 0) (= i 0) (loop (< i 5) (= i (+ i 1)) (if (== i 3) (continue)) (= sum (+ sum i)))` — sum = 12 (1+2+4+5, skip 3).
4. `(each i (vector int 10 20 30) (if (== i 1) (break)))` → exits after first iteration.
5. `(break)` at top level → error "break outside of loop".
6. `(continue)` at top level → error "continue outside of loop".
7. `(break 42)` → arity error.
8. Break inside nested `do`/`if` propagates to enclosing loop.

---

## 3. Vector resize / reserve

### Design

Add two methods in the vector method dispatch block (bbl.cpp ~line 1648):

**resize**: `(v.resize n)` — sets vector length to `n`.  If growing, zero-fills new elements.  If shrinking, truncates.

```cpp
if (method == "resize") {
    if (node.children.size() < 2) throw BBL::Error{"vector.resize requires a size"};
    BblValue sizeVal = eval(node.children[1], scope);
    if (sizeVal.type != BBL::Type::Int) throw BBL::Error{"vector.resize: size must be int"};
    int64_t n = sizeVal.intVal;
    if (n < 0) throw BBL::Error{"vector.resize: size must be non-negative"};
    vec->data.resize(static_cast<size_t>(n) * vec->elemSize, 0);
    return BblValue::makeNull();
}
```

**reserve**: `(v.reserve n)` — pre-allocates capacity without changing length.

```cpp
if (method == "reserve") {
    if (node.children.size() < 2) throw BBL::Error{"vector.reserve requires a capacity"};
    BblValue capVal = eval(node.children[1], scope);
    if (capVal.type != BBL::Type::Int) throw BBL::Error{"vector.reserve: capacity must be int"};
    int64_t n = capVal.intVal;
    if (n < 0) throw BBL::Error{"vector.reserve: capacity must be non-negative"};
    vec->data.reserve(static_cast<size_t>(n) * vec->elemSize);
    return BblValue::makeNull();
}
```

Zero-filling on resize is valid for all element types: int (0), float (0.0), bool (false), struct (zero-initialized bytes).

### Acceptance

1. `(= v (vector int)) (v.resize 5) (v.length)` → 5
2. `(= v (vector int)) (v.resize 3) (v.at 0)` → 0 (zero-filled)
3. `(= v (vector int 1 2 3 4 5)) (v.resize 2) (v.length)` → 2 (truncated)
4. `(= v (vector int)) (v.reserve 1000) (v.length)` → 0 (length unchanged)
5. `(v.resize -1)` → error
6. `(v.resize "a")` → type error

---

## 4. Script-level Error Handling (try/catch)

### Design

Syntax: `(try body... (catch err-var handler...))`.  The last child of `try` must be a list whose first element is the symbol `catch`.

New special form `Try` — add to enum and lookup table.

Implementation:

```cpp
case SpecialForm::Try: {
    if (node.children.size() < 2)
        throw BBL::Error{"try requires a body and catch clause"};
    // Last child must be (catch err-var handler...)
    auto& catchNode = node.children.back();
    if (catchNode.type != NodeType::List || catchNode.children.empty() ||
        catchNode.children[0].type != NodeType::Symbol ||
        catchNode.children[0].stringVal != "catch")
        throw BBL::Error{"try: last argument must be a (catch err-var handler...) clause"};
    if (catchNode.children.size() < 2)
        throw BBL::Error{"catch requires an error variable name"};
    if (catchNode.children[1].type != NodeType::Symbol)
        throw BBL::Error{"catch: first argument must be a symbol"};

    BblValue result = BblValue::makeNull();
    try {
        for (size_t i = 1; i < node.children.size() - 1; i++) {
            result = eval(node.children[i], scope);
            if (flowSignal) return result;
        }
    } catch (BBL::Error& e) {
        BblScope catchScope;
        catchScope.parent = &scope;
        uint32_t errId = resolveSymbol(catchNode.children[1].stringVal);
        catchScope.def(errId, BblValue::makeString(intern(e.what)));
        activeScopes.push_back(&catchScope);
        result = BblValue::makeNull();
        for (size_t i = 2; i < catchNode.children.size(); i++) {
            result = eval(catchNode.children[i], catchScope);
            if (flowSignal) break;
        }
        activeScopes.pop_back();
    }
    return result;
}
```

The error message string is bound to the error variable.  The catch body runs in a child scope.

### Considerations

- Only `BBL::Error` is caught — C++ exceptions from native code (e.g. `std::bad_alloc`) are NOT caught by script-level try/catch.  This is intentional: script errors are recoverable, system errors are not.
- `catch` is not a reserved keyword / special form itself — it's only valid as the last child of `try`.  Using `(catch ...)` standalone is a function-call lookup that will fail normally.
- The try body can contain multiple expressions.  The catch handler can also contain multiple expressions.

### Acceptance

1. `(= result (try (/ 1 0) (catch e e)))` → result is `"division by zero"` string.
2. `(try (+ 1 2) (catch e "error"))` → 3 (no error, body value returned).
3. `(try (/ 1 0) (catch e (+ "caught: " e)))` → `"caught: division by zero"`.
4. `(try (catch e))` → error: try requires a body.
5. `(try (+ 1 2))` → error: last argument must be catch clause.
6. Nested try: inner catch handles inner error, outer body continues.
7. Error variable `e` is scoped to the catch block — not visible after try/catch.
