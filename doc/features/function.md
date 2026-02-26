# Functions

Functions in BBL are first-class values created with the `fn` special form.  They support closures, recursion, higher-order patterns, and interop with C++ functions registered via `defn()`.

## defining functions

### syntax

```bbl
(fn (param1 param2 ...) body...)
```

`fn` creates an anonymous function value.  The parameter list is a parenthesized list of symbols.  The body is one or more expressions — the function returns the value of the last expression.  There is no explicit `return` keyword.

```bbl
(= double (fn (x) (* x 2)))
(double 5)       // 10

(= greet (fn (name)
    (print "hello " name "\n")
))
(greet "world")
```

### parameters

Parameters are untyped — the caller can pass any type.  The function body discovers types at runtime through the operations it performs.

```bbl
(= double (fn (x) (* x 2)))
(double 5)       // 10 (int)
(double 3.14)    // 6.28 (float)
(double "hi")    // runtime error: * cannot apply to string
```

Arity is enforced at call time.  Passing the wrong number of arguments throws `BBL::Error`.

### return values

Functions return the value of their last evaluated expression.  Since `if` and `loop` are statements (they return `null`), use a result variable pattern for conditional returns:

```bbl
(= abs (fn (x)
    (= result x)
    (if (< x 0)
        (= result (* x -1))
    )
    result
))
```

## closures

A `fn` expression captures **free variables** from the enclosing scope at the time it is evaluated.

### capture semantics

- **Value types** (int, float, bool, null, struct) are copied.  The closure gets its own snapshot.
- **GC-managed types** (string, binary, fn, vector, table, userdata) are shared — the closure holds a reference to the same object.  Mutations to a captured container are visible through the closure.
- **Rebinding** the outer variable after capture does not affect the closure — it holds the value that existed at capture time.

```bbl
// value capture — snapshot at definition time
(= x 10)
(= f (fn () x))
(= x 99)
(f)                  // 10 (captured the old x)

// shared container — mutations visible through closure
(= log (table))
(= record (fn (v) (log.push v)))
(record 1)
(record 2)
(print log.length)   // 2 (closure and outer scope share log)
```

### free variable detection

The interpreter performs a static AST walk (`gatherFreeVars`) over the function body to identify free variables — symbols used in the body that are not:

1. Parameters of this function
2. Assigned by `=` or `def` within the body
3. Parameters of nested `fn` expressions
4. Special forms (`=`, `def`, `set`, `if`, `loop`, `and`, `or`, `fn`, `exec`, `not`)

Each free variable is looked up in the enclosing scope at the time the `fn` expression evaluates.  If found, its current value is stored in the function's capture list.  If not found (e.g., the name will be defined later or is a typo), it is silently skipped — the missing binding will cause an "undefined symbol" error at call time.

### capture storage

Captures are stored as a `vector<pair<string, BblValue>>` on the `BblFn` object.  The GC traces through captures during the mark phase, keeping captured objects alive.

### `=` in closures

`=` walks the scope chain: local → captured → root.  An `=` inside a closure can modify a captured binding:

```bbl
(= make-counter (fn ()
    (= count 0)
    (fn ()
        (= count (+ count 1))
        count
    )
))
(= c (make-counter))
(c)   // 1
(c)   // 2
```

Note: `=` modifies the captured binding inside the closure, not the original scope.

## higher-order functions

Functions are first-class values.  They can be passed as arguments, returned from functions, and stored in tables.

```bbl
// function factory
(= make-adder (fn (n) (fn (x) (+ x n))))
(= add5 (make-adder 5))
(add5 3)             // 8

// function as argument
(= apply-twice (fn (f x) (f (f x))))
(apply-twice add5 10)   // 20

// functions in tables
(= ops (table "add" (fn (a b) (+ a b)) "mul" (fn (a b) (* a b))))
(= add-fn (ops.get "add"))
(add-fn 3 4)            // 7
```

## recursive functions

Functions defined with `(= name (fn ...))` can call themselves recursively.  The interpreter detects when a function's body references its own name as a free variable and injects a self-reference into the function's captures after the `=` completes.

### mechanism

When evaluating `(= name (fn (params) body))`:

1. The `fn` expression evaluates, capturing free variables from the current scope.  At this point, `name` is not yet defined, so it is not captured.
2. `=` binds `name` to the new function in the scope.
3. The interpreter checks: is `name` a free variable in the function's body?  If yes, and `name` is not already in captures, inject `name → fn-value` into the captures.

This enables direct recursion without any special syntax.

### examples

```bbl
// recursive factorial
(= fact (fn (n)
    (= result 1)
    (if (<= n 1)
        (= result 1)
        (= result (* n (fact (- n 1))))
    )
    result
))
(fact 10)   // 3628800

// recursive fibonacci
(= fib (fn (n)
    (= result 0)
    (if (<= n 1)
        (= result n)
        (= result (+ (fib (- n 1)) (fib (- n 2))))
    )
    result
))
(fib 10)    // 55
```

### the result variable pattern

Since `if` is a statement (returns `null`), recursive functions that compute a value must use the result variable pattern:

```bbl
(= result <default>)
(if <condition>
    (= result <base-case>)
    (= result <recursive-case>)
)
result
```

This is the standard BBL idiom for conditional return values.

### mutual recursion

Mutual recursion (function A calls B, B calls A) does **not** work automatically.  The self-capture injection only handles the case where a function references its own name.  For mutual recursion, both functions would need to be defined before either captures the other.  This is a known limitation — use a table to hold mutually recursive functions if needed:

```bbl
(= fns (table))
(fns.set "is-even" (fn (n)
    (= result true)
    (if (== n 0)
        (= result true)
        (= result ((fns.get "is-odd") (- n 1)))
    )
    result
))
(fns.set "is-odd" (fn (n)
    (= result false)
    (if (== n 0)
        (= result false)
        (= result ((fns.get "is-even") (- n 1)))
    )
    result
))
```

## call execution model

When a BBL function is called:

1. **Arity check** — the number of arguments must match the number of parameters.  Mismatch throws `BBL::Error`.
2. **Fresh scope** — a new scope is created with no parent chain.  Closures do not see the caller's variables.
3. **Load captures** — captured variables are loaded into the fresh scope.
4. **Bind arguments** — arguments are bound to parameter names.  If an argument name collides with a capture, the argument wins.
5. **Evaluate body** — body expressions are evaluated sequentially.  The value of the last expression becomes the return value.
6. **Scope cleanup** — the call scope is popped from the active scopes list (used by GC for safety).

The fresh-scope-with-captures model means functions are fully isolated from the call-site environment.  A function only sees its parameters and whatever it captured at definition time.

## C++ function registration

C++ functions are registered with `bbl.defn()` and follow a stack-based calling convention.

```cpp
bbl.defn("name", function_pointer);
```

### C function signature

```cpp
typedef int (*BblCFunction)(BblState* bbl);
```

The function:
1. Reads arguments by index (`bbl->getIntArg(0)`, `bbl->getStringArg(1)`, etc.)
2. Performs work
3. Optionally pushes a return value (`bbl->pushInt(42)`)
4. Returns `0` (no return value) or `1` (one return value pushed)

### example

```cpp
int my_add(BblState* bbl) {
    int64_t a = bbl->getIntArg(0);
    int64_t b = bbl->getIntArg(1);
    bbl->pushInt(a + b);
    return 1;
}

// Register:
bbl.defn("my-add", my_add);
```

```bbl
(print (my-add 3 4))   // 7
```

### argument access

| method | returns | description |
|--------|---------|-------------|
| `argCount()` | `int` | number of arguments |
| `hasArg(i)` | `bool` | whether argument `i` exists |
| `getArgType(i)` | `BBL::Type` | type tag of argument `i` |
| `getIntArg(i)` | `int64_t` | read int argument |
| `getFloatArg(i)` | `double` | read float argument |
| `getBoolArg(i)` | `bool` | read bool argument |
| `getStringArg(i)` | `const char*` | read string argument |
| `getBinaryArg(i)` | `BblBinary*` | read binary argument |
| `getUserDataArg(i)` | `void*` | read userdata argument |
| `getArg(i)` | `BblValue` | read raw tagged value |

### return value methods

| method | description |
|--------|-------------|
| `pushInt(val)` | push int |
| `pushFloat(val)` | push float |
| `pushBool(val)` | push bool |
| `pushString(str)` | push string (interned) |
| `pushNull()` | push null |
| `pushBinary(ptr, size)` | push binary (copies data) |
| `pushUserData(type, ptr)` | push typed userdata |

## performance considerations

### function call overhead

Function calls cost approximately **430 ns** per call (measured with 500K calls, release build, GCC -O2).  This includes:

- Arity check
- Scope allocation (`BblScope` with `unordered_map`)
- Loading captures into the new scope
- Binding arguments
- Evaluating the body AST (tree-walking)
- Popping the active scope (GC bookkeeping)

For comparison, a tight arithmetic loop iteration costs ~280 ns.  The additional ~150 ns per function call comes from scope creation and capture loading.

### closure capture cost

Captures are stored as a flat vector of `{name, BblValue}` pairs.  Creating a closure with N captured variables incurs:

- **Definition time**: O(N) copy of captured values + O(body size) AST walk to find free variables.
- **Call time**: O(N) insertion into the call scope's hash map.

For functions with few captures (typical case), this is negligible.  Functions capturing many variables (>20) may see measurable scope setup time.

### recursive function cost

Each recursive call creates a fresh scope and pushes to the `activeScopes` list.  Deep recursion (thousands of frames) will:

1. Consume C++ stack space (each `callFn` → `eval` chain uses several hundred bytes of stack).
2. Accumulate scope objects that the GC must trace through `activeScopes`.

There is no tail-call optimization.  Deep recursion (>10K frames) risks stack overflow.  For deep iteration, use `loop` instead.

### GC interaction

Function objects and their captures are GC-managed.  The GC traces:

- All `BblFn` objects in allocation pools
- Captures stored on each `BblFn`
- Active call scopes via the `activeScopes` list

GC triggers at safe points only (top of statement execution, loop iterations) — never mid-expression.  This means function calls within an expression are safe from mid-call collection.

### C function cost

C functions registered via `defn()` are stored as `BblCFunction` pointers — the call overhead is the same argument marshaling cost as BBL functions, but without scope creation or capture loading.  C functions are faster for compute-heavy operations.

### optimization guidance

| pattern | cost | recommendation |
|---------|------|----------------|
| Simple function call | ~430 ns | Fine for most use cases |
| Hot loop with function call | ~430 ns/iter | Move logic inline if performance-critical |
| Deep recursion (>1K frames) | Stack risk | Rewrite as `loop` |
| Many captures (>20) | Scope setup overhead | Reduce captured variables |
| C++ function via `defn()` | ~280 ns | Preferred for compute-heavy operations |
