# Basic Binary Lisp Specification

## primary use case

Serialize complex C++ data structures including binary blobs.  Embeddable scripting as a secondary use case.

## goals

- binary data as a first-class citizen
- C++ compatible struct layout
- small implementation (single header or small lib)
- call from C, call to C
- strong performance
- simple tracing GC
- usable by LLM agents with minimum context engineering

## non-goals

- multithreading
- infix math (use prefix: `(+ a b)` — avoids ambiguity with hyphenated identifiers like `my-var`)
- strong typing in function args
- macros
- deterministic destruction (use explicit `close` for resources)

## types

### value types (copied on assignment)

`int` `float` `bool` `null` `struct`

Script values use two numeric types: `int` (64-bit signed integer) and `float` (64-bit IEEE double).  Narrow C types (`int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `float32`) exist only inside struct fields and typed vector elements — they are not script-level types.

`struct` instances are value types with C++-compatible binary layout.  Structs are POD — fields are numeric types, `bool`, and other structs only (no strings, containers, or functions in struct fields).  Structs are defined from C++ via `StructBuilder`, not from scripts.

### GC-managed types

- `string` — UTF-8 text, interned
- `binary` — raw byte buffer
- `fn` — function / closure
- `vector` — contiguous typed storage for value types and structs
- `table` — heterogeneous key-value container (string and integer keys)
- `userdata` — opaque `void*` with registered type descriptor, methods, and optional destructor

All GC-managed types are shared on assignment (same object, not a copy).

## syntax

### basics

S-expressions.  First element is the operator or function.  Comments are `//`.

```bbl
(print "hello")       // function call
(= x 10)              // create or rebind a variable
(+ x 1)               // prefix math
```

### `=` (assign-or-create)

`=` is the universal assignment form.  If the name already exists in the scope chain (local → captured → root), `=` rebinds it.  If not found, `=` creates a new binding in the current scope.

```bbl
(= x 10)              // creates x (not yet defined)
(= x 20)              // rebinds x (already exists)
(= y (+ x 1))         // creates y = 21
```

For place expressions (struct fields, table keys), `=` writes to the location:

```bbl
(= player.hp 80)      // write to table key or struct field
```

### lexer rules for literals

Symbols never start with a digit.  All numeric and binary literals start with a digit.  The lexer dispatches on the first characters:

| prefix | meaning | example |
|--------|---------|--------|
| `0b` | binary blob | `0b4096:...` |
| digit + `.` | float | `3.14` |
| digit | int | `42` |
| `"` | string | `"hello"` |

Symbols start with a letter, `_`, or `-` (after first char, digits are allowed too).

No integer suffixes.  There is one integer type (`int`, 64-bit signed).  No hex literals — add a `hex` stdlib function if needed later.

### binary data

Binary blob literal: `0b<size>:<raw bytes>`.  Size is a decimal byte count.  The lexer reads the size, reads `:`, then consumes exactly that many raw bytes (no escaping, no interpretation).  There is no terminator — the size tells the lexer where the blob ends.

The lexer reads all bytes immediately into memory.  No lazy loading.

```bbl
(= blob 0b4096:<4096 raw bytes>)
(= texture 0b65536:<65536 bytes of png data>)
```

### structs

See [structs.md](structs.md) for details.

Structs are registered from C++ via `StructBuilder`.  There is no script-level `struct` keyword.  Scripts use struct types as constructors and access fields via `.`:

```bbl
(= v (vertex 1.0 2.0 3.0))
(print v.x)
```

Structs are **value types** with C++ compatible binary layout.  POD only — no strings, containers, or functions in struct fields.  This guarantees `memcpy` copy semantics and zero-copy `getVector<T>()` from C++.

Structs compose:

```bbl
(= tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
```

### vectors

Contiguous typed storage for value types and structs.

```bbl
(= verts (vector vertex (vertex 0 1 0)))
(verts.push (vertex 1 0 0))
(verts.push (vertex -1 0 0))
```

### tables

Heterogeneous key-value container.  Replaces both ordered lists and hash maps.  Keys are strings or integers.  Values can be any type.

Constructed with alternating key-value pairs (string-keyed) or sequential values (integer-indexed):

```bbl
// string-keyed (like a map)
(= player (table "name" "hero" "hp" 100 "alive" true))

// integer-indexed (like a list)
(= items (table 1 "sword" 2 "shield" 3 "potion"))
```

Access via `.` for string keys:

```bbl
(print player.name)     // "hero"
(= player.hp 80)
```

Dynamic key access via `get` and `set` methods:

```bbl
(= key "hp")
(print (player.get key))   // 80
(player.set key 60)
```

Integer-indexed access via `at` (0-based positional):

```bbl
(print (items.at 0))       // "sword"
```

### functions

Functions are first-class values created with `fn`.  They return the value of their last evaluated expression.  There is no explicit `return` keyword.  Functions support closures, recursion, and higher-order patterns.

```bbl
(= greet (fn (name)
    (print "hello " name "\n")
))
(greet "world")

(= double (fn (x) (* x 2)))   // returns (* x 2)
```

See [features/function.md](features/function.md) for the full function specification: closures, capture semantics, recursion, C++ function registration, and performance considerations.

### closures

A `fn` expression captures free variables from the enclosing scope at the time it is evaluated.

- **Value types** are copied.  The closure gets its own snapshot.
- **GC-managed types** (string, binary, fn, containers, userdata) are shared — the closure holds a reference to the same object.  Mutations to a captured container are visible through the closure.
- **Rebinding** the outer variable after capture does not affect the closure — it holds the value that existed at capture time.

```bbl
// value capture
(= x 10)
(= f (fn () x))
(= x 99)
(f)                  // 10 (captured the old x)

// higher-order function
(= make-adder (fn (n) (fn (x) (+ x n))))
(= add5 (make-adder 5))
(add5 3)             // 8
```

See [features/function.md](features/function.md) for recursive functions and the complete capture algorithm.

### operators

**Arithmetic**: `+` `-` `*` `/` `%` — prefix.  `int + int = int`, `float + float = float`, `int + float = float`.  Three promotion rules.

`+` is variadic for strings — multiple string arguments are concatenated left-to-right: `(+ "hello" " " "world")` → `"hello world"`.  When the left operand is a string, non-string operands are auto-coerced: `(+ "score=" 42)` → `"score=42"`.  `+` where the left operand is numeric and right is string is a runtime error.

**Comparison**: `==` `!=` `<` `>` `<=` `>=` — produce `bool`.  Numeric comparison promotes.  String `==`/`!=` use interned pointer equality (O(1)).  Ordering operators on strings are a type error.

**Logical**: `and` `or` `not`
- `and` and `or` are **short-circuit special forms** — the second operand is not evaluated if the first determines the result.
- `not` is a regular function (prefix): `(not (== x 0))`.
- All three operate on `bool` values only.  Passing a non-bool is a type error.

**Bitwise**: deferred to backlog.

### truthiness

Conditions in `if`, `loop`, and `and`/`or` must evaluate to `bool`.  Non-bool values in conditions are a type error.  There is no implicit truthiness — `0`, `null`, and `""` are not falsy; they are simply not bool.

### special forms

| form | purpose |
|------|--------|
| `=` | assign-or-create: rebind if exists, create if not; also place writes |
| `do` | evaluate multiple expressions sequentially, return last value |
| `loop` | while-loop |
| `each` | index iteration over vector or table |
| `if` | conditional branching |
| `and` | short-circuit logical AND |
| `or` | short-circuit logical OR |
| `fn` | function definition (creates new scope, captures free variables) |
| `execfile` | run another `.bbl` file |
| `exec` | evaluate a string as BBL code |

### control flow

`loop`, `if`, `each`, and `do` are special forms — their bodies run in the enclosing scope.

`do` evaluates multiple expressions sequentially and returns the value of the last one:

```bbl
(= result (do (= a 10) (= b 20) (+ a b)))   // result = 30
```

`if` takes a condition, a then-body, and an optional else-body.  Each branch is a single expression — use `do` to group multiple expressions:

```bbl
(if (== x 0)
    (print "zero")
    (print "nonzero"))

(if (> hp 0)
    (do (= alive true) (print "alive\n"))
    (do (= alive false) (print "dead\n")))
```

`if` is a statement — it does not return a value.  To compute a value conditionally, assign in both branches:

```bbl
(= label "other")
(if (== choice 0) (= label "zero"))
(if (== choice 1) (= label "one"))
```

`loop` is a statement — it does not return a value.

### iteration

`each` iterates over a vector or table, binding an index variable from 0 to length-1:

```bbl
(each i verts
    (= v (verts.at i))
    (print v.x "\n"))
```

The container is evaluated once.  `each` is a statement (returns `null`).  After the loop, the index variable equals the container length.

For non-standard iteration (custom start, step, or condition), use `loop`:

```bbl
(= i 0)
(loop (< i (verts.length))
    (= v (verts.at i))
    (print v.x "\n")
    (= i (+ i 1))
)
```

Table iteration via `keys`:

```bbl
(= player (table "name" "hero" "hp" 100))
(= ks (player.keys))
(each i ks
    (= k (ks.at i))
    (print k ": " (player.get k) "\n"))
```
```

### vector index access

`at` reads vector and table elements by index.  `set` writes.  Integer dot syntax (`v.0`) is sugar for both:

```bbl
(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))
(print (verts.at 0).x)                    // read via method
(print verts.0.x)                         // read via integer dot
(verts.set 0 (vertex 5 5 5))             // write via method
(= verts.0 (vertex 5 5 5))               // write via integer dot
```

### member access — the `.` operator

The `.` operator provides field access and method dispatch on typed values.  `v.x` is syntactic sugar for `(. v x)`.

Field read:

```bbl
(= v (vertex 1.0 2.0 3.0))
(print v.x)
```

Field write:

```bbl
(= v.x 5.0)
```

Method call — `.` looks up a function registered on the value's type:

```bbl
(file.write "hello")
```

See [features/method-resolution.md](features/method-resolution.md) for the full resolution algorithm.

### place expressions

`=` and `set` accept **place expressions** — expressions that identify a writable location.

Place writes are **single-level only** — one dot or one `at`, not chained:

| pattern | meaning |
|---------|---------|
| `symbol` | assign variable: `(= x 5)` |
| `obj.field` | struct field or table string-key: `(= v.x 5)` or `(= player.hp 80)` |
| `obj.N` | integer dot — vector or table integer-key: `(= v.0 val)` or `(= t.0 val)` |

For deeper mutation, use intermediate variables:

```bbl
(= v verts.0)
(= v.x 5.0)
(= verts.0 v)
```

### method dispatch

The `.` operator dispatches methods via **type descriptors**, not vtable pointers.  Every `BblValue` carries a type tag.  The type tag maps to a type descriptor in a global table on the `BblState`.  The descriptor holds the method table.

`(file.write "hello")` → type tag of `file` → type descriptor → method table → `write` → call with `file` as implicit first arg.

See [features/method-resolution.md](features/method-resolution.md) for the full resolution algorithm, per-type rules, and error cases.

Built-in container methods:

| type | methods |
|------|---------|
| `vector` | `push`, `pop`, `clear`, `length`, `at` |
| `table` | `get`, `set`, `delete`, `has`, `keys`, `length`, `push`, `pop`, `at` |
| `string` | `length` (more in string library — deferred) |
| `binary` | `length` |

Built-in free functions (registered via `addStdLib`):

| function | purpose |
|----------|---------|
| `print` | print values to stdout (no trailing newline) |
| `str` | convert any value to its string representation |

### execfile / exec

Two special forms for loading code:

**`execfile`** — runs another `.bbl` file.  **`exec`** — evaluates a string as BBL code.

#### execfile

**From script** — `(execfile "file.bbl")`: creates a fresh isolated scope.  The exec'd file cannot see the caller's variables.  Returns the value of the last evaluated expression in the file.

```bbl
(= scene (execfile "scene.bbl"))
(print scene.name)
```

**From C++** — `bbl.execfile("file.bbl")`: accumulates into the existing root scope.  Each call sees everything previous calls defined.  This is how multi-file workflows share definitions.

**Path resolution**: `execfile` paths are resolved relative to the calling script's directory.  C++ `bbl.execfile()` resolves relative to the process's CWD (or a configurable base path on `BblState`).

**Sandboxing**: from script, `execfile` can only access files in the calling script's directory or child directories.  No absolute paths, no `..`.  The sandbox chains down — an exec'd file's sandbox root is its own directory.  See [features/security.md](features/security.md).

#### exec

**From script** — `(exec "(print 42)")`: evaluates the string as BBL code.  Runs in a fresh isolated scope.  Returns the value of the last evaluated expression.

```bbl
(= result (exec "(+ 1 2)"))
(print result)   // 3
```

**From C++** — `bbl.exec("(= x 10)")`: evaluates the string in the existing root scope.  Accumulates like `bbl.execfile()` but takes source code instead of a filename.

## runtime typing

All values are dynamically typed.  Every value carries a type tag at runtime (tagged union).  There is no compile-time type checking.

Arithmetic operators inspect operand type tags: `int + int = int`, `float + float = float`, `int + float = float`.  Incompatible types (e.g. `int + string`) are a runtime error.

Function arguments are untyped — the caller can pass any type.  The function body discovers types at runtime through the operations it performs.

```bbl
(= double (fn (x) (* x 2)))
(double 5)       // 10 (int)
(double 3.14)    // 6.28 (float)
(double "hi")    // runtime error: * cannot apply to string
```

## ownership model

Simple mark-and-sweep garbage collector.  See [memory-model.md](memory-model.md) for full details.

**One scope type**: a symbol → value table.  Fresh vs shared is the only distinction:
- `fn` call → fresh scope.  Sees: own frame + captured variables from the defining scope.
- `(execfile "file.bbl")` from script → fresh root scope.  Isolated.  Returns the last evaluated expression.
- `(exec "code")` from script → fresh scope.  Returns the last evaluated expression.
- C++ `bbl.execfile()` / `bbl.exec()` → **accumulates** into the existing root scope.
- `loop` / `if` → shared.  Runs inside the current frame.

**GC**: all heap objects (strings, binaries, closures, containers, userdata) are managed by a mark-and-sweep collector.  Assignment is a pointer copy.  The GC runs periodically.  `~BblState` frees everything.

**Value types** (int, float, bool, null): copied inline.  No GC involvement.
**Structs**: value-type shell.  C++ compatible binary layout.  POD only — `memcpy` on copy.

**Type descriptors**: always global (registered on `BblState`).  Never freed during script execution.

## string interning

All strings are interned in a global table on the `BblState`.  Creating a string hashes it and returns the existing instance if found.  This makes string comparison O(1) (pointer equality) and deduplicates storage.

Strings are GC-managed.  Each `intern()` call that creates a new string increments the allocation counter, triggering GC when the threshold is reached.  During the sweep phase, unreachable strings are removed from the intern table and freed.  This keeps the intern table clean even in loops that produce many temporary strings (e.g. `(+ s "x")` in a loop).

### String methods

String values support 16 methods accessible via both dot-access (`s.upper`) and call form (`(s.at 0)`).  Methods: `length`, `upper`, `lower`, `trim`, `trim-left`, `trim-right`, `at`, `slice`, `find`, `contains`, `starts-with`, `ends-with`, `replace`, `split`, `join`, `pad-left`, `pad-right`.  See [bbl.md](bbl.md) for full signatures.

### Parsing builtins

`int` and `float` builtins convert strings to numeric types.  They reject partial parses and overflow.  `int` truncates floats toward zero.  `float` converts integers exactly.

### Format builtin

`fmt` performs string formatting with `{}` placeholders, `{{`/`}}` for literal braces.  Throws on argument count mismatch.

## command-line usage

Run a script:

```sh
bbl script.bbl
```

Interactive mode (no arguments):

```sh
bbl
```

Interactive mode reads s-expressions and evaluates them immediately.  Multi-line input: the REPL waits for balanced parentheses before evaluating.  An open `(` without a matching `)` continues to the next line.

```
> (print "hello")
hello
> (= greet (fn (name)
.     (print "hi " name)
. ))
> (greet "world")
hi world
```

The prompt switches from `>` to `.` while inside an incomplete expression.

See [features/cli.md](features/cli.md) for the full CLI specification (options, script arguments, exit codes, environment variables).

## C++ API

See [features/cpp-api.md](features/cpp-api.md) for the full API specification.

Lua-style embedding.  A `BblState` owns the entire runtime — scope, types, interned strings.

```cpp
BblState bbl;
BBL::addStdLib(bbl);              // register print, file I/O, math
bbl.defn("my-func", my_func);    // register custom C function
bbl.execfile("setup.bbl");       // execute script — env persists
bbl.execfile("scene.bbl");       // second script sees setup.bbl's definitions

auto* verts = bbl.getVector<vertex>("player-verts");  // introspect env
auto* tex = bbl.getBinary("player-texture");
// ~BblState deallocates all script data
```

Key properties:
- `execfile()` does not reset the environment.  Scripts accumulate into the same root scope.
- `exec()` evaluates a code string — also accumulates into the root scope from C++.
- C functions follow `int fn(BblState* bbl)` — read args by index, push return value, return 0 or 1.
- All errors throw `BBL::Error` (see [features/errors.md](features/errors.md)).
- After `execfile()` / `exec()`, C++ can read any variable via typed getters (`getInt`, `getFloat`, `getString`, `getVector<T>`, etc.).

## serialization workflow

The primary use case: dump C++ structs to `.bbl` files, read them back.

```bbl
// scene.bbl — a serialized scene
// vertex and mesh types registered from C++ before exec

(= player-mesh (mesh 1 "hero"))
(= player-verts (vector vertex
    (vertex 0 1 0)
    (vertex 1 0 0)
    (vertex -1 0 0)
))
(= player-texture 0b65536:<65536 bytes of png>)
```

## open questions

None — all resolved.

## document index

Design documents linked from this spec:

| document | topic |
|----------|-------|
| [structs.md](structs.md) | struct layout, C++ registration, composition |
| [memory-model.md](memory-model.md) | GC, scope rules, capture semantics |
| [glossary.md](glossary.md) | BBL terminology and Lisp concepts not used |
| [backlog.md](backlog.md) | deferred features and future work |
| [features/cpp-api.md](features/cpp-api.md) | C++ embedding API (`BblState`, getters, StructBuilder) |
| [features/errors.md](features/errors.md) | error types, conditions, backtrace |
| [features/function.md](features/function.md) | functions, closures, recursion, C++ interop, performance |
| [features/binary-data.md](features/binary-data.md) | binary blob literals, immediate loading |
| [features/vector.md](features/vector.md) | typed contiguous vector |
| [features/table.md](features/table.md) | heterogeneous key-value table |
| [features/string.md](features/string.md) | string type, interning, operations |
| [features/stdlib.md](features/stdlib.md) | standard library (print, file I/O, math) |
| [features/method-resolution.md](features/method-resolution.md) | `.` operator dispatch algorithm, per-type rules, error cases |
| [features/cli.md](features/cli.md) | `bbl` command-line tool, REPL, options, exit codes |
| [features/security.md](features/security.md) | sandboxing, path restrictions, capability model |
