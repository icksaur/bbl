# BBL memory model

## model: one scope type, two rules

There is only **one kind of scope** — a binding environment (a symbol → value table).

The two rules are about when a scope is **fresh** vs **shared**:

| construct | scope |
|-----------|-------|
| `fn` call | **fresh** — new scope for args and locals.  Also holds captured variables. |
| `(execfile "file.bbl")` from script | **fresh** — isolated, sees nothing from the caller.  Returns last expression. |
| `(exec "code")` from script | **fresh** — isolated scope.  Returns last expression. |
| C++ `bbl.execfile()` / `bbl.exec()` | **shared** — accumulates into the existing root scope |
| `loop`, `if` | **shared** — no new scope, runs inside the current frame |

"Global" is not a special concept.  It just means the **root scope of the current script** — the outermost fresh scope.  It's the same data structure as a function's local scope.

## garbage collection

BBL uses a simple **mark-and-sweep** garbage collector.

**Managed objects**: strings, binaries, closures (`fn`), vectors, tables, userdata.

**Roots**: the scope chain (all live bindings from root scope through current call stack), the intern table, and the type descriptor table.

### collection cycle

1. **Mark**: starting from roots, traverse all reachable objects and set a mark bit.
2. **Sweep**: walk the allocation list.  Unmarked objects are freed (destructors run for userdata).  Marked objects have their mark bit cleared.

### when it runs

The GC runs periodically — triggered by allocation pressure (e.g. after N bytes allocated since last collection).  It also runs during `~BblState` to free all remaining objects.

### implications

- **No deterministic destruction.**  Objects are freed at GC time, not when they go out of scope.  Resources like file handles must be closed explicitly: `(f.close)`.  Userdata destructors run eventually but not at a predictable time.
- **No cycle problems.**  Circular references (A→B→A) are handled naturally — the GC only frees unreachable objects.
- **Assignment is cheap.**  Assigning a GC-managed value is a pointer copy.  No reference counting overhead.

## closures

`fn` produces a **function value** that captures free variables from the enclosing scope at the time it is evaluated.

**Capture rules:**
- **Value types** (int, float, bool, null, struct): copied.  The closure gets its own snapshot.
- **GC-managed types** (string, binary, fn, vector, table, userdata): the closure holds a reference to the same object.  Mutations through the closure are visible to all holders.
- **Rebinding** the outer variable after capture does not affect the closure — it holds the value that existed at capture time.

```
bbl.execfile("script.bbl")
  └─ root scope of script.bbl
       ├─ (= x 10)                     ← root-scope binding
       ├─ (= make-adder (fn (n)        ← defines outer fn
       │     (fn (x) (+ x n))            ← inner fn captures n from make-adder's frame
       │  ))
       └─ (= add5 (make-adder 5))      ← add5 is a fn with n=5 in its capture env
```

The captured environment is stored as a `{name, BblValue}[]` on the `fn` object.  The GC traces through it during mark phase.

### `=` semantics with captures

`=` uses **assign-or-create** semantics: it checks local → captured → root for an existing name.  If found, it rebinds that slot.  If not found, it creates a new binding in the current scope.

```bbl
(= x 10)
(= f (fn ()
    (= x 20)     // modifies captured x
))
(f)
// x is still 10 here — the closure modified its own snapshot (x is a value type)
```

For GC-managed types, `set` in a closure makes the new object visible anywhere the binding is active — but rebinding a captured name does not affect the original scope.

## functions return their last expression

Both `fn` bodies and `execfile`'d / `exec`'d code return the value of their last evaluated expression.  There is no explicit `return` keyword.

```bbl
(= double (fn (x) (* x 2)))

(= classify (fn (x)
    (= label "other")
    (if (< x 0) (= label "negative"))
    (if (== x 0) (= label "zero"))
    label
))
```

## structs: pure value types

Struct instances are **value types** — their data layout is C++ compatible.  Structs are POD: fields are numeric types, bool, or other structs.  No strings, containers, or functions in struct fields.

Copying a struct is `memcpy`.  No GC involvement in struct operations.

```bbl
(= a (vertex 1.0 2.0 3.0))
(= b a)         // b is a copy — memcpy
(= b.x 9.0)    // does not affect a
```

## type descriptors: global lifetime

Type descriptors live in a global table on `BblState` and are **never freed during script execution**.  They hold:
- Field name → byte offset table (structs)
- Method name → C function table (userdata)

Type descriptors are registered from C++ via `StructBuilder` or `TypeBuilder`.  There is no script-level type definition.

## scope for execfile'd / exec'd code

**From script** — `(execfile "other.bbl")` creates a new **root scope** (not inheriting from the caller).  The exec'd file runs like a fresh script and returns the value of its last expression.  Its root scope is destroyed when it returns.

`(exec "code")` from script also creates a fresh scope.  The string is parsed and evaluated in isolation.  Returns the last expression.

**From C++** — `bbl.execfile("other.bbl")` and `bbl.exec("code")` both accumulate into the existing root scope.  The second call sees everything the first call defined.

## string interning

All strings are interned in a global table on `BblState`.  Creating a string hashes it and returns the existing instance if found.  String comparison is O(1) (pointer equality).

Interned strings live for the **lifetime of `BblState`** — they are never evicted from the intern table.  `~BblState` frees the entire table.  This avoids coupling the intern table to the GC and keeps the implementation simple.

For a serialization DSL that runs, loads data, and exits, the memory cost is negligible.
