# Bracket Binary Lisp Specification

## primary use case

Serialize complex C++ data structures including binary blobs.  Embeddable scripting as a secondary use case.

## goals

- binary data as a first-class citizen
- C++ compatible struct layout
- small implementation (single header or small lib)
- call from C, call to C
- strong performance
- deterministic object lifetimes (RAII, no GC)
- usable by LLM agents with minimum context engineering

## non-goals

- multithreading
- infix math (use prefix: `[+ a b]` — avoids ambiguity with hyphenated identifiers like `my-var`)
- strong typing in function args
- macros

## types

### value types (copied on assignment)

`uint8` `uint16` `uint32` `uint64` `int8` `int16` `int32` `int64` `f32` `f64` `bool` `null` `userdata`

`userdata` is an opaque `void*` pointer (like Lua's lightuserdata).  Scripts can store and pass it but cannot dereference it.  Two flavors: **plain** (value type, no methods, no destructor — C++ owns the lifetime) and **typed** (refcounted, with methods and optional destructor registered via `TypeBuilder`).  Both are `BBL::Type::UserData`.  `File`, `Socket`, etc. are typed userdata.

### reference types (refcounted, shared on assignment)

- `string` — UTF-8 text, interned (see string interning)
- `binary` — raw byte buffer
- `fn` — function

### containers

- `vector` — contiguous memory for value types, like C++ `std::vector`
- `map` — hash-based key-value lookup (string and integer keys only)
- `list` — heterogeneous ordered collection of tagged values (`std::vector<BblValue>` internally)

## syntax

### basics

Bracket expressions.  First element is the operator or function.  Comments are `//`.

```bbl
[print "hello"]       // function call
[= x 10]             // assignment (also definition)
[+ x 1]              // prefix math
```

### lexer rules for literals

Symbols never start with a digit.  All numeric and binary literals start with a digit.  The lexer dispatches on the first characters:

| prefix | meaning | example |
|--------|---------|--------|
| `0x` | hex integer | `0xFF` |
| `0b` | binary blob | `0b4096:...` |
| digit + `.` | float (f64 default) | `3.14` |
| digit | integer (int32 default) | `42` |
| digit + suffix | sized integer | `42u`, `42ul`, `42i64` |
| `"` | string | `"hello"` |

Symbols start with a letter, `_`, or `-` (after first char, digits are allowed too).

Integer literal suffixes (C-style):

| suffix | type |
|--------|------|
| (none) | `int32` |
| `u` | `uint32` |
| `l` | `int64` |
| `ul` | `uint64` |
| `i64` | `int64` |

Examples: `42`, `42u`, `42l`, `42ul`, `42i64`, `0xFFu`

Smaller numeric types (`int8`, `int16`, `uint8`, `uint16`, `f32`) have no literal suffix.  They exist only as struct fields and typed vector elements.  Script literals use wider types (`int32`, `f64`); values are truncated following C narrowing rules on assignment to typed storage.

### binary data

Binary blob literal: `0b<size>:<raw bytes>`.  Size is a decimal byte count.  The lexer reads the size, reads `:`, then consumes exactly that many raw bytes (no escaping, no interpretation).  There is no terminator — the size tells the lexer where the blob ends.

Lazy-load: the interpreter records the file offset and byte count, skips forward.  Bytes are only read into memory when the value is accessed.

```bbl
[= blob 0b4096:<4096 raw bytes>]
[= texture 0b65536:<65536 bytes of png data>]
```

Hex integer literal:

```bbl
[= header 0xDEADBEEF]
```

### structs

See [structs.md](structs.md) for the full discussion.  Decisions:

Structs are **value types** with C++ compatible binary layout.  The struct type is a value that acts as a constructor.

```bbl
[= vertex [struct [f32 x f32 y f32 z]]]
[= v [vertex 1.0 2.0 3.0]]
```

Structs compose:

```bbl
[= triangle [struct [vertex a vertex b vertex c]]]
```

Member functions are defined inline and stored in the **type descriptor**, not in the instance.  `this` is an implicit first argument.

```bbl
[= Foo [struct [i32 n]
    [= inc [fn [this]
        [= this.n [+ this.n 1]]
        this
    ]]
]]
[= foo [Foo 0]]
[= foo [foo.inc]]    // return-and-rebind pattern for mutation
[print foo.n]        // 1
```

`sizeof` a struct contains only data fields — member functions are on the type, not the instance.

### vectors

Contiguous typed storage for value types and structs.

```bbl
[= verts [vector vertex [0 1 0]]]
[verts.push [1 0 0]]
[verts.push [-1 0 0]]
```

### maps

Key-value container.  Keys are strings or integers.  Values can be any type.  String keys are the common case.  Constructed with alternating key-value pairs.

```bbl
[= player [map "name" "hero" "hp" 100 "alive" true]]
```

Access via `.` for string keys (same syntax as structs):

```bbl
[print player.name]     // → "hero"
[= player.hp 80]        // set
```

Dynamic key access via `get` and `set` methods:

```bbl
[= key "hp"]
[print [player.get key]]   // → 80
[player.set key 60]
```

This gives Lua-table-like flexibility with zero new syntax — maps reuse `.` for the common case and use `.get`/`.set` methods for the dynamic case.  When a map key collides with a built-in method name (e.g. a key named `"length"`), the method wins — use `.get` for dynamic access to such keys.

### functions

Functions return the value of their last evaluated expression.  There is no explicit `return` keyword.

```bbl
[= greet [fn [name]
    [print "hello " name "\n"]
]]
[greet "world"]

[= double [fn [x] [* x 2]]]   // returns [* x 2]
```

### closures

A `fn` expression captures free variables from the enclosing scope at the time it is evaluated.

- **Value types** are copied.  The closure gets its own snapshot.
- **Reference types** (`string`, `binary`, `fn`) and **containers** (`vector`, `map`, `list`) have their refcount incremented.  The closure shares the same object.  Mutations to a captured container are visible through the closure.
- **Rebinding** the outer variable after capture does not affect the closure — it holds the value that existed at capture time.

```bbl
// value capture
[= x 10]
[= f [fn [] x]]
[= x 99]
[f]                  // → 10 (captured the old x)

// higher-order function
[= make-adder [fn [n] [fn [x] [+ x n]]]]
[= add5 [make-adder 5]]
[add5 3]             // → 8

// shared container
[= log [list]]
[= record [fn [v] [log.push v]]]
[record 1]
[record 2]
[print log.length]   // → 2 (closure and outer scope share log)
```

The captured environment is stored as a `{name, BblValue}[]` on the `fn` object.  Freed when the `fn` refcount reaches zero.

### operators

**Arithmetic**: `+` `-` `*` `/` `%` — prefix, as with all bracket expressions.  Operands follow C promotion rules with one change: signed + unsigned promotes to signed at the wider width (e.g. `int32 + uint32 → int64`) to avoid silent unsigned wrapping.

`+` is variadic for strings — multiple string arguments are concatenated left-to-right: `[+ "hello" " " "world"]` → `"hello world"`.  The result is a new interned string.  `+` on incompatible types (e.g. `int + string`) is a runtime error.

**Comparison**: `==` `!=` `<` `>` `<=` `>=` — produce `bool`.  Numeric comparison follows promotion rules.  String `==`/`!=` use interned pointer equality (O(1)).  Ordering operators (`<` `>` `<=` `>=`) on strings are a type error — use a library function for lexicographic ordering if needed.

**Logical**: `and` `or` `not`
- `and` and `or` are **short-circuit special forms** — the second operand is not evaluated if the first determines the result.
- `not` is a regular function (prefix): `[not [== x 0]]`.
- All three operate on `bool` values only.  Passing a non-bool is a type error.

**Bitwise**: deferred to backlog.

### truthiness

Conditions in `cond`, `loop`, and `and`/`or` must evaluate to `bool`.  Non-bool values in conditions are a type error.  There is no implicit truthiness — `0`, `null`, and `""` are not falsy; they are simply not bool.

### special forms

These look like bracket expressions but the interpreter handles them specially.  Their bodies execute in the enclosing scope (like C statements), not in a new scope.

| form | purpose |
|------|--------|
| `=` | assignment / definition |
| `loop` | while-loop |
| `for` | iteration |
| `cond` | conditional branching (expression — returns a value) |
| `and` | short-circuit logical AND |
| `or` | short-circuit logical OR |
| `fn` | function definition (creates new scope, captures free variables) |
| `struct` | type definition |
| `exec` | run another file |

### control flow

`loop`, `for`, and `cond` are special forms — their bodies run in the enclosing scope, like C `while`/`for`/`if`.

```bbl
[= i 0]
[loop [< i 10]
    [print i "\n"]
    [= i [+ i 1]]   // modifies enclosing scope's i
]
```

`cond` is an **expression** — it evaluates to the value of the taken branch's last expression.  This is how you conditionally compute a value:

```bbl
[= label [cond
    [[== choice 0] "zero"]
    [[== choice 1] "one"]
    [else          "other"]
]]
```

If no branch matches and there is no `else`, `cond` evaluates to `null`.

`else` is a global constant bound to `true`.  Any truthy bool expression works as a default branch, but `else` is conventional.

`loop` and `for` are statements — they do not return values.

### for loops

`for` iterates over containers.  The loop variable and body are in the enclosing scope.

```bbl
[= verts [vector vertex [0 1 0] [1 0 0] [-1 0 0]]]
[for v verts
    [print v.x "\n"]
]
```

Map iteration via `keys`:

```bbl
[= player [map "name" "hero" "hp" 100]]
[for k [player.keys]
    [print k ": " [player.get k] "\n"]
]
```

### vector index access

`at` accesses vector elements by index.  Writable.

```bbl
[= verts [vector vertex [0 1 0] [1 0 0]]]
[print [verts.at 0].x]                    // read
[= [verts.at 0] [vertex 5 5 5]]          // write whole element
```

This avoids ambiguity with `.` — dot is always field/method by name, `at` is always by index.  `at` is a method on vector/list types.

### member access — the `.` operator

The `.` operator provides field access and method dispatch on typed values.  `v.x` is syntactic sugar for `[. v x]`.

Field read:

```bbl
[= v [vertex 1.0 2.0 3.0]]
[print v.x]              // sugar for [. v x]
```

Field write — `[= v.x 5.0]` desugars to a field-set at parse time:

```bbl
[= v.x 5.0]
```

Method call — `.` looks up a function registered on the value's type:

```bbl
[file.write "hello"]     // sugar for [. file write "hello"]
```

This avoids global name collisions.  `write` doesn't need to be a global — it's looked up on `file`'s type.  The same per-type table used for struct fields also holds named methods, so the incremental implementation cost is small.

The desugaring is consistent: `v.x` always becomes `[. v x]`.  Assignment works the same way:

```bbl
[= v.x 1]         // sugar for [= [. v x] 1]
[= [. v x] 1]     // explicit form — identical behavior
```

### place expressions

`=` is a special form that accepts **place expressions** on the left side — expressions that identify a writable location.

Valid places:

| pattern | meaning |
|---------|---------|
| `symbol` | bind to variable: `[= x 5]` |
| `obj.field` | struct/map field: `[= v.x 5]` |
| `[. obj field]` | explicit dot: `[= [. v x] 5]` |
| `[obj.at index]` | container index: `[= [verts.at 0] val]` |

### method dispatch

The `.` operator dispatches methods via **type descriptors**, not vtable pointers.  Every `BblValue` carries a type tag.  The type tag maps to a type descriptor in a global table on the `BblState`.  The descriptor holds the method table.

`[file.write "hello"]` → type tag of `file` → type descriptor → method table → `write` → call with `file` as implicit first arg.

See [features/method-resolution.md](features/method-resolution.md) for the full resolution algorithm, per-type rules, and error cases.

Struct binary data stays pure C++ layout — no vtable pointer, no metadata embedded in the struct.  Methods are metadata on the type, not on the instance.

Built-in container methods:

| type | methods |
|------|---------|
| `vector` | `push`, `pop`, `clear`, `length`, `at` |
| `map` | `get`, `set`, `delete`, `has`, `keys`, `length` |
| `list` | `push`, `pop`, `clear`, `length`, `at` |
| `string` | `length` (more in string library — deferred) |

### import/exec

Exec runs another `.bbl` file.  Two distinct behaviors depending on context:

**From script** — `[exec "file.bbl"]`: creates a fresh isolated scope.  The exec'd file cannot see the caller's variables.  Returns the value of the last evaluated expression in the file.

```bbl
[= scene [exec "scene.bbl"]]
[print scene.name]
```

**From C++** — `bbl.exec("file.bbl")`: accumulates into the existing root scope.  Each call sees everything previous calls defined.  This is how multi-file workflows share definitions.

**Path resolution**: `exec` paths are resolved relative to the calling script's directory.  If `/foo/a.bbl` does `[exec "sub/b.bbl"]`, and `b.bbl` does `[exec "c.bbl"]`, then `c.bbl` resolves to `/foo/sub/c.bbl`.  C++ `bbl.exec()` resolves relative to the process's CWD (or a configurable base path on `BblState`).

### `=` semantics

`=` serves three roles: create a new binding, rebind an existing name, and modify a place (struct field, vector element, map key).

Resolution order (lookup-then-assign):
1. Check the current scope chain (local → captured → root) for an existing binding with the given name.
2. If found, **rebind** that slot (update the value, adjust refcounts).
3. If not found, **create** a new binding in the current (innermost) scope.

This means `[= x 10]` inside a closure that captured `x` modifies the captured value, not creates a new local.  To intentionally shadow, there is no mechanism — choose distinct names.

For captured value types, the modification only affects the closure's snapshot (it was copied at capture time).  For captured reference types, the modification is visible to all holders of the same reference.

## runtime typing

All values are dynamically typed.  Every value carries a type tag at runtime (tagged union).  There is no compile-time type checking.

Arithmetic operators (`+`, `-`, `*`, `/`, `%`, comparison) inspect operand type tags, promote if needed, and error on incompatible types (e.g. `int + string` → runtime error).  Promotion follows C implicit conversion rules with one exception: signed + unsigned promotes to **signed at the wider width** (e.g. `int32 + uint32 → int64`).  This avoids the C footgun where `-1 + 1u` produces a huge unsigned number.

Math always operates at the promoted width.  Narrowing only occurs on assignment to typed storage (struct field, typed vector element).  This means `[+ uint8_val 300]` produces an int32 result — no mid-expression wrapping.  Truncation happens when the result is stored back into a uint8 slot.

Function arguments are untyped — the caller can pass any type.  The function body discovers types at runtime through the operations it performs.

```bbl
[= double [fn [x] [* x 2]]]
[double 5]       // → 10 (int)
[double 3.14]    // → 6.28 (f64)
[double "hi"]    // → runtime error: * cannot apply to string
```

## ownership model

No GC.  One scope type, two rules.  Refcounting for reference types.  See [memory-model.md](memory-model.md) for full details.

**One scope type**: a symbol → value table.  Fresh vs shared is the only distinction:
- `fn` call → fresh scope.  Sees: own frame + captured variables from the defining scope (see closures).  NOT the caller's locals.
- `[exec "file.bbl"]` from script → fresh root scope.  Isolated.  Returns the last evaluated expression.
- C++ `bbl.exec()` → **accumulates** into the existing root scope (or creates one on first call).  This is how multi-file workflows share definitions.
- `loop` / `for` / `cond` → shared.  Runs inside the current frame.

**Refcounting**: all heap objects are refcounted.  Assignment increments the new refcount and decrements the old.  Return values are held alive through the call chain and freed when unused.  Destruction is deterministic and immediate at zero refcount.

**Value types** (integers, floats, bool, null, userdata): copied inline.  Userdata is pointer-width, no ownership.
**Structs**: value-type shell.  C++ compatible binary layout.  Reference-type fields hold refcounted handles; copying a struct increments those refcounts (shallow copy).

```bbl
[= a [mesh 1 "hero"]]
[= b a]              // b is a copy; a.name and b.name share the same string
[= b.name "villain"] // b.name rebinds; a.name is unaffected
```

**Type descriptors**: always global (registered on `BblState`).  Defining a struct inside a function registers a global type.  Never freed during script execution.

## string interning

All strings are interned in a global table on the `BblState`.  Creating a string hashes it and returns the existing instance if found.  This makes string comparison O(1) (pointer equality) and deduplicates storage.  Interning is transparent to the user.

Interned strings are **refcounted**.  When a string's refcount drops to zero (no variable, struct field, capture, or container holds it), the string is removed from the intern table and its memory is freed.  This keeps the intern table bounded by the number of live string references, not the total number of strings ever created.

## command-line usage

The `bbl` binary runs scripts and provides an interactive mode.

Run a script:

```sh
bbl script.bbl
```

Interactive mode (no arguments):

```sh
bbl
```

Interactive mode reads bracket expressions and evaluates them immediately.  Multi-line input: the REPL waits for balanced brackets before evaluating.  An open `[` without a matching `]` continues to the next line.

```
> [print "hello"]
hello
> [= greet [fn [name]
.     [print "hi " name]
. ]]
> [greet "world"]
hi world
```

The prompt switches from `>` to `.` while inside an incomplete expression.

## C++ API

See [features/cpp-api.md](features/cpp-api.md) for the full API specification.

Lua-style embedding.  A `BblState` owns the entire runtime — scope, types, file handles, interned strings.

```cpp
BblState bbl;
BBL::addStdLib(bbl);              // register print, file I/O, math
bbl.defn("my-func", my_func);    // register custom C function
bbl.exec("setup.bbl");           // execute script — env persists
bbl.exec("scene.bbl");           // second script sees setup.bbl's definitions

auto* verts = bbl.getVector<vertex>("player-verts");  // introspect env
auto* tex = bbl.getBinary("player-texture");
// ~BblState deallocates all script data
```

Key properties:
- `exec()` does not reset the environment.  Scripts accumulate into the same root scope.
- C functions follow `int fn(BblState* bbl)` — read args by index, push return value, return 0 or 1.
- All errors throw `BBL::Error` (see [features/errors.md](features/errors.md)).
- After `exec()`, C++ can read any variable via typed getters (`getInt32`, `getString`, `getVector<T>`, etc.).

## serialization workflow

The primary use case: dump C++ structs to `.bbl` files, read them back.

```bbl
// scene.bbl — a serialized scene
[= vertex [struct [f32 x f32 y f32 z]]]
[= mesh [struct [uint32 id string name]]]

[= player-mesh [mesh 1 "hero"]]
[= player-verts [vector vertex
    [0 1 0]
    [1 0 0]
    [-1 0 0]
]]
[= player-texture 0b65536:<65536 bytes of png>]
```

## resolved design decisions

- **Refcount cycles**: self-referential insertion (A into A) is detected and errors at runtime.  Indirect cycles (A→B→A) are solved by **weak references** — a non-owning handle that doesn't increment the refcount.  When the strong refcount reaches zero the object is freed and outstanding weak refs become `null`.  See backlog for weak reference design.
- **Struct copy semantics**: shallow copy (Swift model).  Copying a struct with reference-type fields increments those refcounts.  Rebinding a copy's field does not affect the original.
- **`at` canonical form**: method only — `[verts.at 0]`.  No free-function `at`.
- **Struct mutation**: return-and-rebind pattern for v1 — `[= foo [foo.inc]]`.  `box` (heap-wrapping for pass-by-reference) deferred to backlog.
- **Struct redefinition**: re-registering a struct with an identical layout is a silent no-op.  Different layout is a runtime error.  This allows multiple `exec` of a shared types file.
- **`else`**: a global constant bound to `true`.  Used as the default branch in `cond`.
- **`this` and arity**: arity checking counts user-visible arguments only.  Implicit `this` in method dispatch is excluded from the count.  `[fn [this] ...]` called via `[foo.inc]` = 0 user args.
- **String interning**: refcounted.  When a string's refcount drops to zero, it is removed from the intern table and freed.  The table is bounded by live references.
- **Binary from non-seekable sources**: binary literals parsed from stdin/pipes are read immediately into memory (loaded state).  Binary literals from files are lazy-loaded (unloaded state, seek-on-access).  Both expose the same `BblBinary` API.
- **Place expression depth**: unlimited.  A place chain is a sequence of pointer chases — each link resolves to a reference into the parent container or struct.  The final link is a set (for writes) or a copy + refcount increment (for reads).  No special depth limit or write-back cascade is needed; you're always writing through the resolved pointer.  `[= [verts.at 0].pos.x 5.0]` and `[= [[m.get "verts"].at 0].x 5.0]` both work.
- **Vectors of structs with reference fields**: allowed.  Copying a struct element out of a vector increments refcounts on its reference-type fields (normal shallow-copy rule).  Moving an element (e.g. `pop`, internal reallocation) transfers ownership without touching refcounts.  `getVector<T>()` exposes raw memory — zero-copy GPU handoff only works for POD structs with no reference-type fields; this is a documented limitation, not a prevented case.
- **CType merged into userdata**: `CType` is not a separate type.  Typed userdata (with methods and destructor) and plain userdata (bare pointer) are both `BBL::Type::UserData`.  The difference is whether a type descriptor is attached.
- **Narrow numeric types**: `int8`, `int16`, `uint8`, `uint16`, `f32` have no literal suffix.  Values are created by assignment to typed storage (struct fields, typed vector elements); wider literals are truncated following C narrowing rules.
- **String `+` arity**: variadic.  Multiple string arguments are concatenated left-to-right.
- **`bbl.exec()` from C++**: returns `void`.  On error, throws `BBL::Error`.  The `BblState` remains valid for cleanup and introspection after a throw.
- **Implicit return**: functions and `exec`'d files return the value of their last evaluated expression.  There is no explicit `return` keyword.

## open questions

None — all resolved.

## document index

Design documents linked from this spec:

| document | topic |
|----------|-------|
| [structs.md](structs.md) | struct layout, composition, member functions |
| [memory-model.md](memory-model.md) | ownership, refcounting, scope rules, capture semantics |
| [glossary.md](glossary.md) | BBL terminology and Lisp concepts not used |
| [backlog.md](backlog.md) | deferred features and future work |
| [features/cpp-api.md](features/cpp-api.md) | C++ embedding API (`BblState`, getters, StructBuilder) |
| [features/errors.md](features/errors.md) | error types, conditions, backtrace |
| [features/binary-data.md](features/binary-data.md) | binary blob literals, lazy loading, seekable vs non-seekable |
| [features/vector.md](features/vector.md) | typed contiguous vector |
| [features/map.md](features/map.md) | hash map with string/integer keys |
| [features/list.md](features/list.md) | heterogeneous ordered list |
| [features/string.md](features/string.md) | string type, interning, operations |
| [features/stdlib.md](features/stdlib.md) | standard library (print, file I/O, math, socket) |
| [features/method-resolution.md](features/method-resolution.md) | `.` operator dispatch algorithm, per-type rules, error cases |
