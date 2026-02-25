# Glossary

BBL terminology and how it relates to (or differs from) traditional Lisp concepts.

## BBL terms

| term | meaning |
|------|---------|
| **s-expression** | `(op args...)` — the fundamental syntax unit. |
| **special form** | An s-expression handled directly by the interpreter rather than as a function call.  `def`, `set`, `if`, `loop`, `fn`, `exec`, `and`, `or`. |
| **place expression** | The target of `(set place val)` — an expression that identifies a writable location.  A symbol (`x`), a dot access (`v.x`), or an indexed access (`(verts.at 0)`).  Single-level only — no chaining like `(set a.b.c val)`. |
| **binding** | An entry in a scope table: a name associated with a value.  `(def x 5)` creates a binding; `(set x 10)` rebinds it. |
| **scope** | A symbol → value table.  There is one scope type.  It is either **fresh** (new table, e.g. `fn` call) or **shared** (runs in the enclosing table, e.g. `loop`, `if`). |
| **root scope** | The outermost scope of a script.  What other languages call "global."  Not special — same data structure as a function's local scope. |
| **capture** | When a `fn` is evaluated, free variables from the enclosing scope are copied into the `fn` value's environment.  Value types are snapshotted; GC-managed types are shared (same object). |
| **value type** | A type that is copied on assignment: `int`, `float`, `bool`, `null`, structs.  No heap allocation, no GC involvement. |
| **GC-managed type** | A type managed by the garbage collector and shared on assignment: `string`, `binary`, `fn`, `vector`, `table`, `userdata`. |
| **container** | A GC-managed collection: `vector` (typed contiguous) or `table` (heterogeneous key-value). |
| **type descriptor** | A global table entry on `BblState` that holds a type's method table (for GC types) or field layout (for structs).  Instances carry a type tag, not a vtable pointer. |
| **type tag** | An integer on every `BblValue` identifying its type.  Maps to a `BBL::Type` enum value.  Used for runtime dispatch. |
| **method dispatch** | `v.method` resolves: type tag of `v` → type descriptor → method table → call with `v` as implicit first argument. |
| **interning** | All strings are stored in a global table on `BblState` keyed by content hash.  Duplicate strings share one allocation.  Pool-lifetime — freed when `~BblState` runs. |
| **`userdata`** | An opaque `void*` with a registered type descriptor (methods and optional destructor via `TypeBuilder`).  All userdata is typed.  `File` etc. are userdata. |
| **`BblState`** | The C++ object that owns the entire BBL runtime: scopes, GC heap, type descriptors, intern table.  One state per interpreter instance. |
| **`exec`** | Runs another `.bbl` file.  From script: isolated scope, returns last expression.  From C++: accumulates into existing root scope. |

## Lisp concepts NOT used in BBL

| Lisp concept | BBL equivalent or absence |
|--------------|---------------------------|
| **cons cell / pair** | None.  BBL uses `table` (heterogeneous) and `vector` (typed contiguous storage). |
| **car / cdr** | None.  Use `(t.at 0)` and iteration.  No linked-list primitives. |
| **quote / quasiquote** | None.  BBL has no code-as-data or macro system.  Data is constructed via constructors (`table`, `vector`, struct types). |
| **macro** | Explicit non-goal.  No compile-time code transformation. |
| **symbol (as data)** | Symbols exist only as variable names in source code.  Not a runtime data type.  Use strings where Lisp would use symbols as data. |
| **nil** | `null` — a value type, not a special list terminator. |
| **atom** | No distinction.  All values are tagged unions.  No atom/list dichotomy. |
| **lambda** | `fn` — `(fn (args...) body...)`.  Same role, different keyword, has closures. |
| **let / set! / define** | `def` creates a new binding.  `set` rebinds an existing one.  Two forms instead of three. |
| **tail call optimization** | Not guaranteed.  Not a design goal for v1. |
| **continuation** | None.  No `call/cc`.  Control flow is `if`, `loop` only. |
| **garbage collection** | Mark-and-sweep tracing GC — standard approach. |
| **multiple return values** | Not in v1 (deferred to backlog).  Functions return one value (last expression). |
| **apply / funcall** | Functions are called by position in an s-expression: `(f args...)`.  No separate `apply`. |
| **eval** | No runtime eval of strings as code.  `exec` runs files, not strings. |
