# overkill review

This is a review of features that fail the complexity check in this project's specification.  We suggest cuts that decrease the project implementation complexity by significant chunks.

The BBL spec as written is more complex than Lua 5.x to implement while trying to do less (no GC, no metatables, no coroutines).  Lua is ~20K lines of C.  The complexity in BBL comes from C++ interop (contiguous typed vectors, struct binary layout) — that's the core value prop.  Everything else should be as simple as possible.

---

## goal-preserving suggestions

### 1. merge list and map into table

**Complexity cut: large.**

The spec has three container types: `vector` (typed contiguous), `list` (heterogeneous ordered), `map` (hash key-value).  `list` and `map` are both heterogeneous containers backed by `BblValue` elements.  A single `table` type (Lua-style: integer-indexed contiguous portion + string-keyed hash portion) replaces both.

What you delete:
- One entire container implementation (list)
- One type tag, one dispatch path in method resolution
- One set of C++ API functions (`BblList`, `getList()`)
- Separate `list` and `map` documentation, tests, error messages

What you keep:
- `vector` (typed contiguous) — still needed for serialization / GPU handoff
- `table` — does everything list and map did

`[list 1 "two" 3.0]` becomes `[table 1 "two" 3.0]` (integer-indexed).  `[map "name" "hero"]` becomes `[table "name" "hero"]` (string-keyed).  Mixed access works: `[= t [table "x" 10]]` then `[t.push "extra"]`.  This is how Lua works and Lua tables are one of the simplest yet most powerful container designs ever made.

The vector/table split is clean: vector is for C++ interop (contiguous typed memory), table is for scripting.  Two containers, not three.

### 2. two script-level numeric types instead of twelve

**Complexity cut: large.**

The spec defines 10 numeric types (`int8`–`int64`, `uint8`–`uint64`, `f32`, `f64`) as runtime type tags in `BblValue`.  This means:
- The tagged union has 10 numeric variants
- Arithmetic promotion rules must handle all pairs (100+ combinations)
- Comparison must handle all pairs
- The C++ API needs getters/setters for each

The spec already says small types have no literal suffix and "exist only as struct fields and typed vector elements."  Take this to its conclusion:

**Script values are always `int64` or `f64`.  Period.**

- Literal `42` → int64.  Literal `3.14` → f64.
- `BblValue` has two numeric tags: `Int` and `Float`.
- Arithmetic: int+int=int, float+float=float, int+float=float.  Three rules.
- Struct fields still use narrow C types (`f32 x`, `uint8 r`).  On read from a struct field or typed vector, values are widened to int64/f64.  On write, values are narrowed (C truncation rules, same as current spec).
- Drop all integer literal suffixes (`u`, `l`, `ul`, `i64`).  No need — there's one integer type.
- Drop `0x` hex literals.  If needed, add a `hex` stdlib function later.

The `BBL::Type` enum goes from 20+ entries to ~12.  The promotion logic goes from a matrix to three lines.  The C++ argument/return API goes from 10 typed getters to 2 (`getInt`, `getFloat`) plus convenience wrappers that cast.

Struct layout and vector element storage are unchanged — they still use narrow C types internally.  The simplification is entirely in the scripting layer.

### 3. drop lazy binary loading — always read immediately

**Complexity cut: medium.**

Lazy loading requires:
- Two internal states for `BblBinary` (loaded/unloaded) with transition logic
- `BblSourceFile` refcounted file handle management
- Seek tracking across multiple blobs per source file
- Keeping file handles open for the lifetime of unloaded blobs
- Special handling when blobs outlive their source file's exec scope

Without lazy loading:
- The lexer reads `0b<size>:` and immediately reads `size` bytes into a buffer
- `BblBinary` is always a `{uint8_t* data, size_t length}` with refcount
- No file handles, no seek, no state transitions

Binary data is still a first-class citizen.  The serialization use case still works.  You just pay memory for all blobs at parse time.

If a scene file has 100MB of textures, yes, they all load.  But this is an optimization problem, not a language problem.  The lazy loading can be added later as a pure interpreter optimization with zero language-level changes — it's invisible to scripts.  For v1, read everything.

This also means `filebytes` becomes trivial: `stat` the file, read it, return a binary.

### 4. pool-lifetime string interning — don't evict

**Complexity cut: small-to-medium.**

The spec says interned strings are refcounted and evicted from the intern table when their refcount hits zero.  This means:
- Every string assignment must increment/decrement the intern table entry's refcount
- Zero-refcount triggers removal from the hash table
- The intern table interacts with the general refcounting machinery

Simpler: all interned strings live for the lifetime of `BblState`.  Never evict.  This is how many language runtimes handle interning.

- String creation: hash, check table, return existing or insert.  Done.
- String destruction: nothing.  `~BblState` frees the whole table.
- String comparison: still O(1) pointer equality.

The cost is leaked memory for temporary strings created by `+` concatenation.  For a serialization DSL that runs, loads data, and exits, this is negligible.  If a long-running script generates millions of unique temporary strings, it leaks.  But that's not the use case.

This cuts the refcount machinery from every string operation and eliminates the refcount/intern-table coupling.

### 5. drop cycle detection

**Complexity cut: medium.**

The spec says self-referential insertion (`[m.set "self" m]`) is detected and errors at runtime.  This means every `push`, `set`, and container-modifying operation must check whether the value being inserted is (or contains) the container itself.  For indirect cycles, the spec defers to weak references (also complex).

Alternative: don't detect.  A refcount cycle is a memory leak.  Document it: "don't create cycles."  This is what CPython does for refcounted objects without the optional cycle collector — cycles leak.

For a serialization DSL where data is tree-shaped (scenes, meshes, configs), cycles essentially never happen.  If they do, the memory leaks until `~BblState` cleans everything up.  This is fine.

This removes: cycle detection logic from every container mutation, the weak reference concept entirely, and a category of error messages.

### 6. drop struct member functions

**Complexity cut: medium.**

Struct member functions with value-type `this` create the return-and-rebind pattern (`[= foo [foo.inc]]`), which is confusing and unlike any mainstream language.  The implementation requires:
- Parsing `fn` definitions inside `struct` and routing them to the type descriptor's method table
- Distinguishing field names from method names at definition time (collision detection)
- Implicit `this` injection during method dispatch
- Special arity-counting rules (exclude `this`)
- The `.` operator resolving to both fields AND methods on structs

If structs are pure data (fields only, no methods):
- Struct definition is just a field list: `[= vertex [struct [f32 x f32 y f32 z]]]`
- The `.` operator on structs only resolves fields.  One lookup, no ambiguity.
- "Methods" are free functions: `[= vertex-length [fn [v] [sqrt [+ [* v.x v.x] [* v.y v.y] [* v.z v.z]]]]]`
- Method dispatch via `.` is only for typed userdata (File, Socket) and containers — types registered from C++, where methods make sense.

This fits the serialization use case perfectly: structs are data.  Functions that operate on structs are just functions.  The OOP-like dispatching on structs is scripting convenience that costs a lot of implementation complexity for value-type semantics that are awkward anyway.

### 7. drop socket stdlib from v1

**Complexity cut: small.**

The socket stdlib is 2 typed userdata types (`Socket`, `ServerSocket`), 7 methods, destructor management, blocking I/O semantics.  It's scope creep for a serialization-focused language.  If someone needs sockets, they register a C function.

### 8. unify userdata to one flavor

**Complexity cut: small.**

The spec has "plain" userdata (value type, no methods, no destructor) and "typed" userdata (refcounted, methods, destructor).  Two code paths for the same concept.

Just have typed userdata.  If you don't need methods/destructor, register a type with an empty method table and no destructor.  One code path.  The "plain" flavor is just the degenerate case.

If you want the opposite simplification: only plain userdata.  C++ manages everything.  No `TypeBuilder`, no destructor dispatch.  File/Socket become C functions that return plain userdata and require explicit `[close f]` calls.  This is larger scope decrease but eliminates typed userdata entirely.

### 9. single-level place expression writes

**Complexity cut: small-to-medium.**

The spec allows arbitrarily deep write-through chains: `[= [verts.at 0].pos.x 5.0]`.  Each link resolves to a pointer into the parent.  This requires the evaluator to build a chain of "place references" and write through them.

If writes are limited to single-level: `[= v.x 5.0]` and `[= [verts.at 0] val]` work, but `[= [verts.at 0].x 5.0]` does not.  For deep mutation, use intermediate variables:

```bbl
[= v [verts.at 0]]
[= v.x 5.0]
[= [verts.at 0] v]
```

Three lines instead of one, but the evaluator only needs to handle: symbol assignment, single-dot field write, and single `at` write.  No recursive place resolution.

### 10. `if` instead of `cond`

**Complexity cut: small.**

`cond` is a multi-branch expression that returns a value.  Replace with `if` as a statement:

```bbl
[= label "other"]
[if [== choice 0] [= label "zero"]]
[if [== choice 1] [= label "one"]]
```

Or with `if`/`else`:

```bbl
[if [== x 0]
    [print "zero"]
    [print "nonzero"]
]
```

The evaluator doesn't need to thread return values through branching logic.  The `else` global constant trick goes away.  The "null on no match" behavior goes away.

This is a modest savings.  `cond` as expression is ~10 lines of evaluator code.  But it removes a concept from the language.

### 11. `exec` always returns a table, not dual behavior

**Complexity cut: small.**

The spec has `exec` behave differently from script (fresh scope) vs C++ (accumulative).  Instead: `exec` always creates a fresh scope and returns all defined symbols as a table.

```bbl
[= types [exec "types.bbl"]]
[= v [types.vertex 1.0 2.0 3.0]]
```

From C++, `bbl.exec()` continues to accumulate (this is the C++ API, not the `exec` special form).  The script-side `exec` always isolates and returns.  One behavior, no context-dependent dispatch.

Actually the spec already says this.  Confirm: from script, exec creates a fresh scope and returns last expression.  From C++, accumulates.  These are two different call sites (script `exec` vs C++ `bbl.exec()`), so they're already separate.  If this is truly separate code paths, the complexity is already contained.  This suggestion may be a no-op — verify during implementation.

### 12. drop `for` — use `loop` with `at` and `length`

**Complexity cut: small.**

`for` is syntactic sugar:

```bbl
[= i 0]
[loop [< i [verts.length]]
    [= v [verts.at i]]
    [print v.x "\n"]
    [= i [+ i 1]]
]
```

This is more verbose but eliminates a special form.  One less thing to parse, evaluate, document, and test.  `loop` already exists and can do everything `for` does.

---

## goal-modifying suggestions

### A. simple tracing GC instead of refcounting

**Complexity trade: replaces one hard problem with a simpler one.**

Refcounting requires:
- Increment/decrement on every assignment, scope exit, capture, return
- Careful threading of return values through the call chain (the "chain problem" in memory-model.md)
- Cycle detection or weak references
- Correct destructor ordering

A simple mark-and-sweep GC:
- No reference counting anywhere.  Assignment is just a pointer copy.
- No cycle problem — GC handles it.
- Return values "just work" — they're reachable from the stack, so they survive.
- Closures are simpler — captured values are just pointers into heap objects.
- Destructor ordering is non-deterministic (bad for File handles, but explicit `close` works).

This contradicts the "deterministic object lifetimes (RAII, no GC)" goal.  But consider: the RAII goal exists to avoid GC complexity.  A simple mark-and-sweep (Lua's is <500 lines) is arguably less total implementation complexity than correct refcounting with cycle handling.  The trade-off is losing deterministic File/Socket cleanup — but explicit `close` or the C++ API handles that.

### B. structs cannot contain reference-type fields

**Complexity cut: large.  Changes struct capability.**

If structs are POD-only (value types and other structs, no strings/containers/functions in fields), then:
- Struct copy is `memcpy`.  No refcount adjustment, no field-by-field walk.
- Vector of structs is pure `realloc`/`memcpy`.  No refcount bookkeeping on push/pop/realloc.
- `getVector<T>()` is always zero-copy safe.
- The "struct copy increments reference-type field refcounts" rule and all its edge cases vanish.

The cost: `[= mesh [struct [uint32 id string name]]]` is no longer valid.  Name would need to be a separate variable or stored in a table.  This pushes the design toward: structs for numeric/binary data (the serialization case), tables for everything else (the scripting case).

### C. separate `def` and `set`

`=` doing create, rebind, and place-write means the evaluator must walk the scope chain on every assignment to decide which operation to perform.  The "lookup-then-assign" semantics also make it impossible to intentionally shadow a variable.

- `def` creates a new binding in the current scope (always).
- `set` rebinds an existing binding or writes to a place (always).  Error if the name doesn't exist.

```bbl
[def x 10]
[set x 20]
[def x 30]     // shadows — new binding in current scope
[set y 5]      // error: y not defined
```

The evaluator always knows which operation to perform.  No scope-chain walk for `def`.  Error on typos with `set`.  This is how Scheme (`define` vs `set!`), Rust (`let` vs assignment), and most languages work.

### D. use parentheses instead of square brackets

Square brackets have no technical advantage and create friction:
- Every editor, syntax highlighter, and Lisp tool assumes parentheses
- Paredit, rainbow brackets, structural editing — all assume `()`
- LLMs have vastly more training data with `()` Lisp syntax
- The "usable by LLM agents with minimum context engineering" goal is better served by `()`

```bbl
(= vertex (struct (f32 x f32 y f32 z)))
(= v (vertex 1.0 2.0 3.0))
(print v.x)
```

The implementation cost is identical — just a different token.  But the ecosystem cost of `[]` is real.

### E. drop script-defined structs — C++-only registration

If structs can only be defined from C++ via `StructBuilder`, the interpreter doesn't need:
- The `struct` special form
- Runtime type descriptor registration
- Inline function parsing in struct bodies
- Struct redefinition detection
- Struct composition from script

Scripts use structs as opaque constructors.  Types files become C++ setup code instead of `.bbl` files:

```cpp
addVertex(bbl);
addTriangle(bbl);
bbl.exec("scene.bbl");  // scene.bbl uses vertex, triangle — already registered
```

This hurts the "self-contained `.bbl` file" use case.  A `.bbl` data file can't define its own types — the C++ loader must know what types to register.  But for many real workflows (game engines, asset pipelines), the C++ side already knows the types.

---

## summary: suggested v1 cut

If you apply suggestions 1–9 and A or B from above, the language becomes:

- **5 types**: null, bool, int, float, string, binary, fn, table, vector, struct, userdata
- **2 containers**: vector (typed contiguous), table (heterogeneous, replaces list+map)
- **2 numeric types** in script: int64, f64 (narrow types only in struct fields)
- **No lazy binary loading** (read immediately; optimize later)
- **No cycle detection** (document: don't create cycles)
- **No struct methods** (structs are pure data; use free functions)
- **No socket stdlib** (register from C++ if needed)
- **Pool-lifetime strings** (never evict from intern table)
- **Single-flavor userdata** (typed only; empty method table if plain)
- **Single-level place writes** (no deep chains)

This is a language that a single developer can implement in 3–5K lines of C++.