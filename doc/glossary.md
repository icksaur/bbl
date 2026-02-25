# glossary

BBL terminology and how it relates to (or differs from) traditional Lisp concepts.

## BBL terms

| term | meaning |
|------|---------|
| **bracket expression** | `[op args...]` — the fundamental syntax unit.  Equivalent to Lisp's S-expression `(op args...)` but uses square brackets. |
| **special form** | A bracket expression handled directly by the interpreter rather than as a function call.  `=`, `cond`, `loop`, `for`, `fn`, `struct`, `exec`, `and`, `or`. |
| **place expression** | The left side of `[= place val]` — an expression that identifies a writable location.  A symbol (`x`), a dot access (`v.x`), or an indexed access (`[verts.at 0]`).  These can chain: `[= [verts.at 0].x 9.0]`. |
| **binding** | An entry in a scope table: a name associated with a value.  `[= x 5]` creates or rebinds a binding. |
| **scope** | A symbol → value table.  There is one scope type.  It is either **fresh** (new table, e.g. `fn` call) or **shared** (runs in the enclosing table, e.g. `loop`). |
| **root scope** | The outermost scope of a script.  What other languages call "global."  Not special — same data structure as a function's local scope. |
| **capture** | When a `fn` is evaluated, free variables from the enclosing scope are copied into the `fn` value's environment.  Value types are snapshotted; reference types are shared (refcount incremented). |
| **value type** | A type that is copied on assignment: integers, floats, `bool`, `null`, `userdata`, structs.  No heap allocation, no refcounting. |
| **reference type** | A type that is refcounted and shared on assignment: `string`, `binary`, `fn`. |
| **container** | A reference-type collection: `vector`, `map`, `list`.  Refcounted, shared on assignment. |
| **type descriptor** | A global table entry on `BblState` that holds a type's field layout and method table.  Struct instances carry a type tag, not a vtable pointer. |
| **type tag** | An integer on every `BblValue` identifying its type.  Maps to a `BBL::Type` enum value.  Used for runtime dispatch. |
| **method dispatch** | `v.method` resolves: type tag of `v` → type descriptor → method table → call with `v` as implicit first argument. |
| **return-and-rebind** | The pattern for mutating value-type structs: `[= foo [foo.inc]]`.  Since `this` is a copy, member functions return the modified struct, and the caller rebinds the variable. |
| **interning** | All strings are stored in a global table on `BblState` keyed by content hash.  Duplicate strings share one allocation.  Refcounted — freed when no references remain. |
| **lazy loading** | Binary blobs from files record a file offset + size and defer reading until first access.  Binary blobs from non-seekable sources (stdin, pipes) are read immediately into memory. |
| **`userdata`** | An opaque `void*` type (like Lua's combined lightuserdata/userdata).  Two flavors: **plain** (value type, no methods, no destructor) and **typed** (refcounted, with methods and optional destructor via `TypeBuilder`).  Both are `BBL::Type::UserData`.  `File`, `Socket`, etc. are typed userdata. |
| **weak reference** | (Deferred) A non-owning handle that does not increment the referent's refcount.  `[weak x]` creates one; `[deref w]` returns the referent or `null` if freed.  Solves indirect refcount cycles. |
| **`BblState`** | The C++ object that owns the entire BBL runtime: scopes, type descriptors, intern table, file handles.  One state per interpreter instance. |
| **`exec`** | Runs another `.bbl` file.  From script: isolated scope, returns last expression.  From C++: accumulates into existing root scope. |
| **`box`** | (Deferred) Wraps a value-type struct in a refcounted heap allocation for pass-by-reference mutation.  Not in v1. |

## Lisp concepts NOT used in BBL

| Lisp concept | BBL equivalent or absence |
|--------------|---------------------------|
| **S-expression** | Replaced by **bracket expression** `[...]`.  Same nesting semantics, different delimiters. |
| **cons cell / pair** | None.  BBL uses `list` (backed by `std::vector<BblValue>`) and `vector` (typed contiguous storage). |
| **car / cdr** | None.  Use `[list.at 0]` and iteration.  No linked-list primitives. |
| **quote / quasiquote** | None.  BBL has no code-as-data or macro system.  Data is constructed via constructors (`map`, `list`, `struct`). |
| **macro** | Explicit non-goal.  No compile-time code transformation. |
| **symbol (as data)** | Symbols exist only as variable names in source code.  They are not a runtime data type.  Use strings where Lisp would use symbols as data. |
| **nil** | `null` — a value type, not a special list terminator. |
| **atom** | No distinction.  All values are tagged unions.  There is no atom/list dichotomy. |
| **lambda** | `fn` — `[fn [args...] body...]`.  Same role, different keyword, has closures. |
| **let / set! / define** | All three roles are served by `=`.  Lookup-then-assign semantics: rebinds if the name exists, creates a new binding if not. |
| **tail call optimization** | Not guaranteed.  Not a design goal for v1. |
| **continuation** | None.  No `call/cc`.  Control flow is `cond`, `loop`, `for` only. |
| **garbage collection** | Replaced by deterministic refcounting.  Explicit non-goal. |
| **multiple return values** | Not in v1 (deferred to backlog).  Functions return one value (last expression). |
| **apply / funcall** | Functions are called by position in a bracket expression: `[f args...]`.  No separate `apply`. |
| **eval** | No runtime eval of strings as code.  `exec` runs files, not strings. |
