# BBL memory model

## model: one scope type, two rules

There is only **one kind of scope** — a binding environment (a symbol → value table).

The two rules are about when a scope is **fresh** vs **shared**:

| construct | scope |
|-----------|-------|
| `fn` call | **fresh** — new scope for args and locals.  Also holds captured variables. |
| `[exec "file.bbl"]` from script | **fresh** — isolated, sees nothing from the caller.  Returns last expression. |
| C++ `bbl.exec()` | **shared** — accumulates into the existing root scope |
| `loop`, `for`, `cond` | **shared** — no new scope, runs inside the current frame |

"Global" is not a special concept.  It just means the **root scope of the current script** — the outermost fresh scope.  It's the same data structure as a function's local scope.

## closures

`fn` produces a **function value** that captures free variables from the enclosing scope at the time it is evaluated.

**Capture rules:**
- **Value types** (int, float, bool, userdata): copied.  The closure gets its own snapshot.
- **Reference types** (string, binary, fn) and **containers** (vector, map, list): refcount incremented.  The closure shares the same object.  Mutations through the closure are visible to all holders.
- **Rebinding** the outer variable after capture does not affect the closure — it holds the value that existed at capture time.

```
bbl.exec("script.bbl")
  └─ root scope of script.bbl
       ├─ [= x 10]                     ← root-scope binding
       ├─ [= make-adder [fn [n]        ← defines outer fn
       │     [fn [x] [+ x n]]          ← inner fn captures `n` from make-adder's frame
       │  ]]
       └─ [= add5 [make-adder 5]]      ← add5 is a fn with n=5 in its capture env
```

The captured environment is stored as a `{name, BblValue}[]` on the `fn` object.  Freed when the `fn`'s refcount reaches zero.

### `=` semantics with captures

`=` uses **lookup-then-assign**: it checks the scope chain (local → captured → root) for an existing name.  If found, it rebinds that slot.  If not found, it creates a new binding in the current scope.

This means `[= x 10]` inside a closure that captured `x` modifies the captured value, not creates a new local.  For captured value types, this only affects the closure's snapshot.  For captured reference types, the modification is visible through all references.

## refcounting rules

All heap objects (strings, binaries, containers, `fn` values) are refcounted.

| event | effect |
|-------|--------|
| assign to symbol | increment new value's refcount |
| symbol goes out of scope (frame destroyed) | decrement |
| struct field assigned | increment new, decrement old |
| struct copied | increment all reference-type fields |
| closure captures reference type | increment refcount |
| return value from `fn` | increment before frame is destroyed, caller receives |
| unused return value | decremented immediately after expression |
| zero refcount | object destroyed immediately |
| string zero refcount | removed from intern table and freed |

## cycle detection

Self-referential insertion (e.g. `[m.set "self" m]`) is detected and errors at runtime.  Indirect cycles (A→B→A) are solved by **weak references** — `[weak x]` returns a non-owning handle that does not increment the refcount.  When the strong refcount reaches zero the object is freed and outstanding weak refs become `null`.  The programmer breaks a cycle by marking one link as weak (e.g. A→B→weak A).  See [backlog.md](backlog.md) for the full `weak`/`deref` design.

## return values: the chain problem

```bbl
[= make-str [fn [] "hello"]]
[= bar [fn [] [make-str]]]
[print [bar]]
```

Trace:
1. `[bar]` called → new frame
2. `[make-str]` called → returns `"hello"` with refcount incremented for the return
3. `make-str` frame destroyed.  Refcount on `"hello"` still > 0 (held by return temporary)
4. `bar` receives it, returns it (increments refcount for its own return)
5. `bar` frame destroyed.  Refcount still > 0
6. `[print ...]` receives `"hello"` as argument (refcount incremented for arg passing)
7. `print` returns.  Argument refcount decremented.
8. The unused return temporary from `[bar]` is decremented.
9. If no other references, `"hello"` is freed.

With string interning, `"hello"` lives in the intern table for the script lifetime anyway.  This matters for non-interned strings (longer strings, binary data, containers).

The evaluator handles this with a simple rule: **every evaluated subexpression is a refcounted temporary**.  After the parent expression consumes it (assign, pass as arg, return), the temporary is released.

## functions return their last expression

Both `fn` bodies and `exec`'d files return the value of their last evaluated expression.  There is no explicit `return` keyword.

```bbl
[= double [fn [x] [* x 2]]]   // returns [* x 2]
[= classify [fn [x]
    [cond
        [[< x 0] "negative"]
        [[== x 0] "zero"]
        [else "positive"]
    ]                                    // cond is an expression — its value is the fn's return
]]
```

## structs: value shell, refcounted innards

Struct instances are **value types** — their data layout is C++ compatible (no hidden pointers in the struct binary data).  But fields that hold reference types (strings, containers) hold **refcounted handles**.

```bbl
[= mesh [struct [uint32 id string name]]]
[= a [mesh 1 "hero"]]
[= b a]   // copy: id is copied (int32), name's refcount incremented
[= b.name "villain"]  // name's old refcount decremented (may free), new string refcount incremented
[print a.name]  // still "hero" — a.name was a separate handle
```

Deep copy is not automatic.  For containers inside structs, assignment copies the handle (same shared container).  If you want a fresh container, create one explicitly.

## type descriptors: global lifetime

Type descriptors live in a global table on `BblState` and are **never freed during script execution**.  They hold:
- Field name → byte offset table
- Method name → `fn` value table (member functions)

Member functions are stored **on the type, not on instances**.  `[foo.f]` resolves: type tag of `foo` → type descriptor → method `f` → call with `foo` as implicit `this` arg.

**Redefinition**: re-registering a struct with an identical layout is a silent no-op.  A different layout is a runtime error.  This allows multiple `exec` of a shared types file without conflicts.

Because type descriptors are global, `this` is just an ordinary function argument — the standard closure/capture rules apply.

## scope for exec'd files

**From script** — `[exec "other.bbl"]` creates a new **root scope** (not inheriting from the caller).  The exec'd file runs like a fresh script and returns the value of its last expression.  Its root scope is destroyed when it returns.

**From C++** — `bbl.exec("other.bbl")` accumulates into the existing root scope.  The second call sees everything the first call defined.

## the struct mutation trade-off

Structs are value types.  `this` inside a member function is a copy.  Mutation requires return-and-rebind:

```bbl
[= Counter [struct [i32 n]
    [= inc [fn [this]
        [= this.n [+ this.n 1]]
        this    // return modified copy
    ]]
]]
[= c [Counter 0]]
[= c [c.inc]]   // replace c with the returned copy
[print c.n]      // 1
```

The alternative — making structs heap-allocated and refcounted — is deferred as `box` in the backlog.
