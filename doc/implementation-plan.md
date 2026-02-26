# BBL Implementation Plan

## overview

Phased implementation of the BBL interpreter.  Each phase produces a compiled, tested deliverable.  Later phases depend on earlier ones but each is self-contained enough to validate independently.

Build: `cmake -B build && cmake --build build`
Test (unit): `./build/bbl_tests`
Test (functional): `./build/bbl tests/functional/<test>.bbl` (each script prints `PASS` or exits non-zero)

## notes

**GC deferral**: phases 1–4 do not include garbage collection.  All managed objects (strings, closures, vectors, tables) are tracked on an allocation list from phase 1, but periodic sweep is not added until phase 5.3.  Until then, `~BblState` walks the allocation list and frees everything.  This is fine for unit tests (short-lived `BblState` instances) and means an allocation list must exist from phase 1.

**CMake naming**: the static library target is `bbl_lib` (not `bbl`) to avoid collision with the `bbl` CLI executable target.

## test strategy

### unit tests — `tests/test_bbl.cpp`

Test internals and the C++ API directly.  Two categories:

1. **Internal tests** — exercise lexer, parser, and interpreter data structures directly (e.g. token types, AST node construction, scope lookup).
2. **C++ API tests** — create a `BblState`, call `bbl.exec("...")` with script strings, and assert results via `bbl.getInt()`, `bbl.getString()`, etc.  This is the workhorse — most language features are tested this way.

The existing `tests/test_bbl.cpp` harness (`TEST`, `RUN`, `ASSERT_EQ` macros) is used for all unit tests.

### functional tests — `tests/functional/*.bbl`

Small `.bbl` scripts executed by the `bbl` CLI binary.  Each script either:

- Prints `PASS` and exits 0 (success), or
- Prints `FAIL: <reason>` and exits non-zero (failure), or
- Is expected to produce a specific error (validated by a wrapper script).

A shell script `tests/run_functional.sh` runs all `.bbl` files in `tests/functional/` and reports results.

Functional tests validate the CLI tool, `execfile`, file I/O, and end-to-end behavior that cannot be tested through `bbl.exec()` alone.

---

## phase 1 — lexer, parser, core eval

Deliverable: `bbl.h` and `bbl.cpp` compile.  Unit tests pass for tokenization, parsing, and evaluation of basic expressions.

### 1.1 project skeleton

- Create `bbl.h` with the public API: `BblState` class, `BBL::Error`, `BBL::Type` enum, C function typedef.
- Create `bbl.cpp` with stub implementations.
- Update `CMakeLists.txt`: rename library target to `bbl_lib` (`add_library(bbl_lib STATIC bbl.cpp)`).  Add `bbl` CLI executable target (`add_executable(bbl main.cpp)`, `target_link_libraries(bbl PRIVATE bbl_lib)`).  Update test target to link `bbl_lib`.
- Create `main.cpp` — minimal entry point: create `BblState`, load file arg, call `bbl.execfile()`, print errors.
- Verify `cmake -B build && cmake --build build` compiles and `./build/bbl_tests` runs (0 tests).

### 1.2 lexer

Tokenize BBL source into a token stream.  Token types:

| token | examples |
|-------|---------|
| `LParen` | `(` |
| `RParen` | `)` |
| `Int` | `42`, `0`, `-3` |
| `Float` | `3.14`, `0.5` |
| `String` | `"hello"`, `"with \"escapes\""` |
| `Symbol` | `=`, `print`, `my-var`, `+` |
| `Bool` | `true`, `false` |
| `Null` | `null` |
| `Dot` | `.` |
| `Binary` | `0b4096:<raw bytes>` |
| `Eof` | end of input |

Lexer is a `class BblLexer` that takes a `const char*` source and produces tokens on demand (`nextToken()`).  Comments (`//` to end of line) are skipped.  Track line numbers for error reporting.

String escapes: `\"`, `\\`, `\n`, `\t`.

Binary literal: on seeing `0b`, parse decimal size, read `:`, read exactly N bytes into buffer.

Unit tests:
- Tokenize `(+ 1 2)` → `LParen Symbol("+" ) Int(1) Int(2) RParen Eof`
- Tokenize `(= x "hello")` → correct token sequence
- Tokenize `(= y 3.14)` → `Float(3.14)`
- Tokenize `true false null` → `Bool(true) Bool(false) Null`
- Tokenize comments: `(+ 1 // comment\n 2)` → `LParen Symbol Int(1) Int(2) RParen`
- Tokenize string escapes: `"a\"b\\c\n"` → correct string content
- Tokenize negative int: `-3` → `Int(-3)` (but `(- 3)` → `Symbol(-) Int(3)`)
- Tokenize dot: `v.x` — implementation detail: could be `Symbol(v) Dot Symbol(x)` or handled in parser
- Tokenize binary literal: `0b5:hello` → `Binary{data="hello", len=5}`
- Error: unterminated string → `BBL::Error`
- Error: binary literal with insufficient bytes → `BBL::Error`

### 1.3 parser

Parse token stream into an AST.  AST node types:

```cpp
enum class NodeType {
    IntLiteral,     // int64_t value
    FloatLiteral,   // double value
    StringLiteral,  // interned string
    BoolLiteral,    // bool value
    NullLiteral,
    BinaryLiteral,  // raw byte buffer
    Symbol,         // variable reference
    List,           // (op args...) — s-expression
    DotAccess,      // a.b — parsed as a node with lhs + field name
};
```

Parser is a function: `std::vector<AstNode> parse(BblLexer&)`.  Returns a list of top-level expressions (a script is a sequence of s-expressions).

- `(op args...)` → `List` node with children
- `v.x` → `DotAccess` node with `lhs=Symbol("v")`, `field="x"`
- `v.x.y` → `DotAccess` of `DotAccess` (chained reads allowed)
- `(v.method arg)` → `List` where first child is `DotAccess`

Unit tests:
- Parse `42` → `IntLiteral(42)`
- Parse `3.14` → `FloatLiteral(3.14)`
- Parse `"hello"` → `StringLiteral("hello")`
- Parse `(+ 1 2)` → `List[Symbol(+), Int(1), Int(2)]`
- Parse `(= x (+ 1 2))` → nested `List`
- Parse `v.x` → `DotAccess(Symbol(v), "x")`
- Parse `(verts.push (vertex 1 2 3))` → `List` with `DotAccess` head
- Parse multiple top-level expressions → vector of nodes
- Error: unmatched `(` → `BBL::Error` with line number
- Error: unmatched `)` → `BBL::Error` with line number
- Error: empty input → empty vector (valid)

### 1.4 BblValue and type system

The tagged union:

```cpp
struct BblValue {
    BBL::Type type;
    union {
        int64_t intVal;
        double floatVal;
        bool boolVal;
        // pointers for GC types
    };
};
```

- `BBL::Type` enum: `Null, Bool, Int, Float, String, Binary, Fn, Vector, Table, Struct, UserData`
- `BblValue` default-constructs to `Null`
- String representation: pointer to interned `BblString`
- Binary: pointer to `BblBinary { uint8_t* data; size_t length; }`

Unit tests:
- Construct int/float/bool/null values, verify type tags
- Value equality for simple types

### 1.5 string interning

`BblState` owns an intern table: `std::unordered_map<std::string_view, BblString*>`.

- `bbl.intern("hello")` → returns same pointer on second call
- `~BblState` frees all interned strings

Unit tests:
- Intern same string twice → same pointer
- Intern different strings → different pointers

### 1.6 scope and variable lookup

Scope is a `struct BblScope { std::unordered_map<std::string, BblValue> bindings; BblScope* parent; }`.

- `=` assigns to an existing binding or creates one if it doesn't exist.
- `fn` call creates fresh scope.  `loop`/`if` share enclosing scope.

Unit tests (via `bbl.exec()`):
- `(= x 10)` then `bbl.getInt("x")` → 10
- `(= x 10) (= x 20)` → `bbl.getInt("x")` → 20
- `(= y 5)` → creates binding, `bbl.getInt("y")` → 5
- `(= x 1) (= x 2)` → `bbl.getInt("x")` → 2 (shadowing in same scope)

### 1.7 core eval — arithmetic, comparison, logic

Evaluate s-expressions.  Implement:

- Arithmetic: `+`, `-`, `*`, `/`, `%` with int/float promotion
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `and`, `or` (short-circuit special forms), `not` (function)
- `=` (assign-or-create)
- `if` (then-only and then-else forms)
- `loop` (while condition body...)
- String concatenation via `+`

Unit tests (via `bbl.exec()` + getters):
- `(= x (+ 1 2))` → `getInt("x")` = 3
- `(= x (+ 1.0 2.0))` → `getFloat("x")` = 3.0
- `(= x (+ 1 2.0))` → `getFloat("x")` = 3.0 (int→float promotion)
- `(= x (* 3 4))` → 12
- `(= x (/ 10 3))` → 3 (int division)
- `(= x (/ 10.0 3.0))` → ~3.333
- `(= x (% 10 3))` → 1
- `(= x (== 1 1))` → `getBool("x")` = true
- `(= x (< 1 2))` → true
- `(= b (and true false))` → false
- `(= b (or false true))` → true
- `(= b (not true))` → false
- Short-circuit: `(= x 0) (or true (= x 1))` → x stays 0
- String concat: `(= s (+ "a" "b"))` → `getString("s")` = "ab"
- Division by zero: `(/ 10 0)` → throws `BBL::Error` ("division by zero")
- Division by zero (float): `(/ 10.0 0.0)` → throws `BBL::Error`
- Type error: `(+ 1 "hello")` → throws `BBL::Error`
- Type error: `(if 42 (= x 1))` → error (non-bool condition)
- Non-bool in `and`: `(and 1 true)` → type error
- Non-bool in `or`: `(or 0 1)` → type error
- Non-bool in `not`: `(not 42)` → type error
- `if` is a statement: `(= x (if true 42))` → x = null (if does not produce a value)
- `loop` is a statement: `(= x (loop false 1))` → x = null
- `if`: `(= x 0) (if true (= x 1))` → x = 1
- `if` else: `(= x 0) (if false (= x 1) (= x 2))` → x = 2
- `loop`: `(= i 0) (loop (< i 5) (= i (+ i 1)))` → i = 5
- Nested: `(= x (+ (* 2 3) (/ 10 2)))` → 11

### 1.8 functions and closures

- `fn` creates a `BblFn` value (GC-managed) that captures free variables
- Calling a fn creates a fresh scope with args bound, then evaluates body
- Last expression is the return value

Unit tests:
- `(= f (fn (x) (* x 2))) (= r (f 5))` → r = 10
- `(= f (fn (x y) (+ x y))) (= r (f 3 4))` → r = 7
- `(= f (fn () 42)) (= r (f))` → r = 42
- Arity error: `(= f (fn (x) x)) (f 1 2)` → error
- Closure capture (value): `(= x 10) (= f (fn () x)) (= x 99) (= r (f))` → r = 10
- Closure write doesn't leak back (value type): `(= x 10) (= f (fn () (= x 20))) (f)` → `bbl.getInt("x")` = 10 (outer x unchanged — closure modified its own snapshot)
- Closure shared GC object: `(= t (table)) (= f (fn () (t.push 1))) (f)` → table length = 1 (shared container mutation visible) — requires tables from phase 4, defer this test to phase 4
- Higher-order: `(= make (fn (n) (fn (x) (+ x n)))) (= add5 (make 5)) (= r (add5 3))` → r = 8
- Last expression return: `(= f (fn () 1 2 3)) (= r (f))` → r = 3

Closure implementation note: the `fn` value stores its own array of captured `{name, BblValue}` pairs, copied from the enclosing scope at definition time.  When the closure is called, scope lookup is: local bindings → captured array.  This gives value-type snapshot semantics per memory-model.md.

### 1.9 `exec` (string eval)

- `bbl.exec("code string")` from C++ — accumulates into root scope
- `(exec "code")` from script — fresh isolated scope, returns last expression

Unit tests:
- `bbl.exec("(= x 10)")` then `bbl.getInt("x")` → 10
- Two `bbl.exec()` calls — second sees first's definitions
- `(= r (exec "(+ 1 2)"))` → r = 3
- `(= x 99) (= r (exec "(= y 5) y"))` → r = 5, x still 99, y not visible in caller

### validation

- `cmake -B build && cmake --build build` succeeds
- `./build/bbl_tests` — all phase 1 unit tests pass
- Note: `print` and CLI are phase 2.  Phase 1 is validated entirely by unit tests.

---

## phase 2 — `print`, `defn`, C functions, `bbl` CLI

Deliverable: C functions can be registered.  `print` works.  The `bbl` CLI runs scripts and `-e` one-liners.  First functional tests pass.

### 2.1 C function registration — `bbl.defn()`

- `bbl.defn("name", fnPtr)` binds a C function in the root scope
- C function signature: `int fn(BblState* bbl)`
- Arg access: `bbl->getIntArg(idx)`, `bbl->getStringArg(idx)`, `bbl->getFloatArg(idx)`, `bbl->getBoolArg(idx)`, `bbl->getBinaryArg(idx)`, `bbl->getUserDataArg(idx)`, `bbl->getArg(idx)` (generic `BblValue`), `bbl->getArgType(idx)`, `bbl->hasArg(idx)`, `bbl->argCount()`
- Return: typed push methods — `bbl->pushInt(val)`, `bbl->pushFloat(val)`, `bbl->pushBool(val)`, `bbl->pushString(str)`, `bbl->pushNull()`, `bbl->pushBinary(ptr, size)`, `bbl->pushUserData(typeName, ptr)`.  Return 0 (void) or 1 (has return value).

Unit tests:
- Register C function that returns int, call from script, verify result
- Register C function with args, verify arg reading
- `hasArg` with variadic function
- `argCount()` returns correct count
- Arity: C function reading beyond available args → error

### 2.1b C++ setters and introspection

- `bbl.setInt(name, val)`, `bbl.setFloat(name, val)`, `bbl.setString(name, str)`, `bbl.set(name, BblValue)`
- `bbl.has(name)` → bool, `bbl.getType(name)` → `BBL::Type`, `bbl.get(name)` → `BblValue`

Unit tests:
- `bbl.setInt("x", 42)` then `bbl.getInt("x")` → 42
- `bbl.has("x")` → true, `bbl.has("nope")` → false
- `bbl.getType("x")` → `BBL::Type::Int`
- `bbl.setString("s", "hello")` then `bbl.getString("s")` → "hello"

### 2.2 `BBL::addPrint`

- `print` — variadic, prints each arg to stdout (formatting per stdlib.md spec)
- Capture stdout for testing: redirect to a buffer or use a custom writer

Unit tests:
- `(print "hello")` → stdout contains "hello"
- `(print 42)` → "42"
- `(print 3.14)` → formatted float
- `(print true)` → "true"
- `(print null)` → "null"
- `(print "a" "b" "c")` → "abc" (no separator)

### 2.3 `bbl` CLI — `main.cpp`

- `bbl script.bbl` — execute file, exit 0 on success, 1 on error, 2 on file-not-found
- `bbl -e '(print 1)'` — evaluate string
- `bbl -e '...' -e '...'` — multiple `-e` in same environment
- `bbl -v` — print version
- `bbl -h` — print usage
- `bbl` (no args) — REPL (stub: just print prompt and read input for now — full REPL in phase 6)
- `args` table injected for script arguments

### 2.4 `execfile`

- `bbl.execfile("file.bbl")` from C++ — accumulates into root scope
- `(execfile "file.bbl")` from script — fresh scope, returns last expression
- Path resolution: relative to calling script's directory
- Sandbox: reject absolute paths and `..` from script-level calls

Unit tests:
- Write a temp `.bbl` file, `bbl.execfile()` it, verify variables defined
- Two `bbl.execfile()` calls accumulate
- `(execfile "file.bbl")` — returns last expression, caller scope unaffected
- Sandbox: `(execfile "/etc/passwd")` → error
- Sandbox: `(execfile "../escape.bbl")` → error

### 2.5 error reporting and backtrace

- `BBL::Error` with `what` string
- Backtrace: `std::vector<Frame>` on `BblState`, pushed on fn/execfile/exec entry, popped on return
- Print backtrace to stderr on error

Unit tests:
- Error message includes source file and line number
- Backtrace includes function call chain
- Parse error reports line number

### 2.6 functional tests — first batch

Create `tests/functional/` and `tests/run_functional.sh`.

| test file | validates |
|-----------|-----------|
| `hello.bbl` | `(print "PASS\n")` — basic execution |
| `arithmetic.bbl` | compute values, print PASS if all correct |
| `functions.bbl` | define and call functions, closures |
| `if_loop.bbl` | control flow |
| `exec_string.bbl` | `(exec "(+ 1 2)")` returns 3 |

### validation

- `./build/bbl_tests` — all phase 1 + 2 tests pass
- `./build/bbl tests/functional/hello.bbl` prints `PASS`
- `tests/run_functional.sh` — all functional tests pass
- `./build/bbl -e '(print "hello\n")'` prints `hello`

---

## phase 3 — structs and vectors

Deliverable: `StructBuilder` works, struct types are usable from scripts, vectors of structs are contiguous and readable from C++ via `getVector<T>()`.

### 3.1 type descriptors

- Global type descriptor table on `BblState`
- Struct descriptors: field name → (offset, type, size)
- Method descriptors: method name → C function pointer (for vector, table, etc.)
- Register descriptors for ALL built-in types that have methods at this phase: vector (push/pop/clear/length/at), string (length), binary (length).  Table methods are added in phase 4.

### 3.2 `StructBuilder`

- `StructBuilder(name, sizeof)` → `field<T>(name, offset)` → `structField(name, offset, typeName)` → `bbl.registerStruct(builder)`
- After registration: the type name is a callable constructor in scripts
- Validation: overlapping fields, size overflow, re-registration with different layout

Unit tests:
- Register `vertex{float x,y,z}`, construct from script, read fields back
- `(= v (vertex 1.0 2.0 3.0)) (= rx v.x)` → rx = 1.0
- Field write: `(= v.x 5.0)` → v.x = 5.0
- Composed structs: `triangle{vertex a,b,c}`, chained read `tri.a.x`
- Arity error: `(vertex 1.0 2.0)` → error (missing z)
- Type error: `(vertex 1.0 2.0 "three")` → error

### 3.3 vectors

- `BblVector<T>` — contiguous typed storage
- Construction: `(vector vertex (vertex 0 1 0) (vertex 1 0 0))`
- Methods: `push`, `pop`, `clear`, `length`, `at`
- `at` returns a copy (value type).  Writable via place expression: `(= (verts.at 0) val)`
- Type checking on push/at-write: must match declared element type
- C++ getter: `bbl.getVector<vertex>("name")` → direct `T*` pointer

Unit tests:
- Create vector, push elements, verify length
- `at` read and write
- `pop` returns last element
- `clear` resets to 0
- Type mismatch on push → error
- Out-of-bounds `at` → error
- `pop` on empty → error
- `getVector<T>()` returns pointer to contiguous data — read back fields from C++
- `(vector int 1 2 3)` → int vector works too

### 3.4 method dispatch (`.` operator)

- Struct `.` → field lookup
- Vector `.` → method lookup (push, pop, clear, length, at)
- Error on `.` for int, float, bool, null, fn types

Unit tests:
- `v.x` on struct → field value
- `verts.length` → method call returning int
- `(5).foo` → type error
- Unknown method on vector → error
- Unknown field on struct → error

### 3.5 functional tests

| test file | validates |
|-----------|-----------|
| `structs.bbl` | construct, read, write struct fields |
| `vectors.bbl` | construct, push, pop, at, length |

### validation

- All phase 1+2+3 unit tests pass
- C++ test: register vertex, exec script that builds a vector, `getVector<vertex>()` returns correct data
- Functional tests for structs and vectors pass

---

## phase 4 — tables and strings

Deliverable: tables work as heterogeneous key-value containers.  String interning and comparison work.

### 4.1 tables

- `BblTable` — GC-managed, string + integer keys, any-type values
- Construction: `(table "key" val ...)` or `(table 1 val 2 val ...)`
- Methods: `get`, `set`, `delete`, `has`, `keys`, `length`, `push`, `pop`, `at`
- `.` on table: method-first resolution, then string-key access
- Place expression: `(= player.hp 80)` writes string key

Unit tests:
- Construct string-keyed table, read fields via `.`
- Construct integer-indexed table, read via `at`
- `get`/`set`/`delete`/`has`/`keys`/`length`
- `push`/`pop` for integer keys
- Method-first resolution: `t.length` calls method, not key lookup
- Key collision: `(table "length" 42)` → `t.length` returns entry count, `(t.get "length")` returns 42
- `getTable()` from C++ — read back values
- Empty table: `(table)` — length 0
- `(t.get "missing")` → null (absent key)
- `(t.delete "missing")` → no error (no-op)
- `(= t (table "a" 1)) (t.pop)` → error (no integer keys)
- Closure shared GC capture (deferred from phase 1.8): `(= t (table)) (= f (fn () (t.push 1))) (f)` → table length = 1

### 4.2 string methods and comparison

- `s.length` returns byte count
- `==`/`!=` on strings → pointer equality (interning)
- `<`/`>` on strings → type error
- `+` concatenation: `(+ "a" "b" "c")` → "abc"

Unit tests:
- `("hello".length)` → 5
- `(== "a" "a")` → true (interned)
- `(== "a" "b")` → false
- `(< "a" "b")` → type error
- String concat already tested in phase 1, verify multi-arg

### 4.3 functional tests

| test file | validates |
|-----------|-----------|
| `tables.bbl` | construction, access, iteration |
| `strings.bbl` | interning, comparison, concat, length |

### validation

- All unit tests pass through phase 4
- Functional tests for tables and strings pass
- Table iteration via keys works end-to-end

---

## phase 5 — binary data, GC, and stdlib

Deliverable: binary literals parse and load.  GC runs correctly.  Standard library (print, file I/O, math) is complete.

### 5.1 binary data

- Lexer handles `0b<size>:<bytes>` (already tokenized in phase 1 lexer)
- `BblBinary` type: `{uint8_t* data, size_t length}`, GC-managed
- `blob.length` method
- `bbl.getBinary("name")` and `bbl.setBinary("name", ptr, size)` C++ API

Unit tests:
- Parse and evaluate a binary literal, verify length
- `getBinary()` returns correct data pointer and length
- `setBinary()` from C++, read back from script

### 5.2 garbage collector

- Mark-and-sweep on `BblState`
- Managed objects: strings (via intern table), binaries, closures, vectors, tables, userdata
- Roots: scope chain, intern table, type descriptors
- Trigger: allocation pressure (e.g. every N bytes allocated)
- `~BblState` runs final collection

Unit tests:
- Create objects, drop references, trigger GC, verify no crash
- Circular references via tables: create cycle, GC collects correctly
- Closures that capture GC objects survive collection
- Stress test: allocate many small objects in a loop, verify memory doesn't grow unboundedly

### 5.3 userdata — `TypeBuilder`

- `TypeBuilder(name)` → `method(name, fnPtr)` → `destructor(fnPtr)` → `bbl.registerType(builder)`
- Userdata instances created via `bbl->pushUserData(typeName, ptr)`
- Methods dispatched via `.` operator
- Destructor called by GC

Unit tests:
- Register a custom userdata type with methods, call from script
- Destructor runs when GC collects the userdata
- Method on wrong type → error

### 5.4 `BBL::addFileIo` — `fopen`, `filebytes`, and `File` type

`TypeBuilder` (5.4) is a prerequisite — `File` is a userdata type registered via `TypeBuilder`.

#### filebytes

- `filebytes` function: read entire file into `BblBinary`
- Path resolution: relative to calling script's directory
- Sandbox: reject absolute paths and `..` from script-level calls

Unit tests:
- Write a temp file, `(filebytes "file")` → correct binary
- Sandbox violation → error
- File not found → error

#### fopen and File type

- `fopen(path)` / `fopen(path, mode)` → `File` userdata
- File methods: `read`, `read-bytes`, `write`, `write-bytes`, `close`, `flush`
- Destructor closes handle if still open when GC'd
- **`fopen` is NOT sandboxed** — it uses standard C `fopen()` semantics.  This is intentional per security.md.  Only `execfile` and `filebytes` enforce sandbox rules.

Unit tests:
- Write file, read it back → contents match
- `read-bytes` returns binary
- Write then close, reopen and verify
- Write to read-mode file → error

### 5.5 `BBL::addMath`

- All math functions per stdlib.md: sin, cos, tan, asin, acos, atan, atan2, sqrt, abs, floor, ceil, min, max, pow, log, log2, log10, exp
- Constants: `pi`, `e`
- Int args promoted to float

Unit tests:
- `(sqrt 4.0)` → 2.0
- `(sin 0.0)` → 0.0
- `(abs -5)` → 5.0 (promoted)
- `(sqrt -1.0)` → error
- `pi` and `e` are defined and approximately correct

### 5.6 `BBL::addStdLib`

- Convenience: calls `addPrint`, `addFileIo`, `addMath`, `addString`
- Idempotent

Unit tests:
- Call `addStdLib` twice — no error, no duplicate registrations

### 5.7 additional `print` formatting tests

Now that all types exist, test `print` formatting for types introduced after phase 2:

- `(print v)` where v is a vertex → `<struct vertex>`
- `(print verts)` where verts is a vector → `<vector vertex length=3>`
- `(print t)` where t is a table → `<table length=3>`
- `(print blob)` where blob is a binary → `<binary 5 bytes>`
- `(print f)` where f is a fn → `<fn name>` or `<fn anonymous>`

### 5.8 functional tests

| test file | validates |
|-----------|-----------|
| `binary_literal.bbl` | parse 0b literal, check length |
| `filebytes.bbl` | read a file, verify length matches |
| `math.bbl` | exercise math functions |
| `file_io.bbl` | write file, read back, verify |

### validation

- All unit tests pass through phase 5
- GC stress test passes without leaks (run under valgrind if available)
- `filebytes` sandbox rejects `..` paths
- All functional tests pass

---

## phase 6 — CLI completion and security

Deliverable: full CLI (REPL, script args, all options), sandbox enforcement, end-to-end functional test suite.

### 6.1 REPL

- Interactive mode when `bbl` is run with no arguments
- Prompt: `> ` for new expression, `. ` for continuation
- Multi-line: wait for balanced parentheses
- Expression result: print non-null/non-void results
- Errors: print to stderr, continue
- Exit: Ctrl-D or Ctrl-C

### 6.2 script arguments

- `args` table injected into root scope before script executes
- `bbl build.bbl output.obj --verbose` → `args` = `(table 1 "output.obj" 2 "--verbose")`

Unit tests (via CLI):
- Run script that prints `args.length`, verify count matches

### 6.3 sandbox enforcement

- `execfile` from script: reject absolute paths, reject `..`, resolve relative to calling script dir
- `execfile` chaining: exec'd file's sandbox root is its own directory
- `filebytes` from script: same rules
- `exec` from script: inherits caller's sandbox root
- C++ API calls: no sandbox (host is trusted)

Unit tests:
- `(execfile "/absolute/path.bbl")` → error
- `(execfile "../escape.bbl")` → error
- Chain test: main.bbl in dir A execfiles subdir/helper.bbl, helper tries `(execfile "../main.bbl")` → error
- `exec` inherits sandbox: `(exec "(filebytes \"local.txt\")")` → OK from same dir

### 6.4 `BBL_PATH` environment variable

- Colon-separated directory list for `execfile` path resolution
- Falls back to script-relative resolution

Functional test:
- Set `BBL_PATH=tests/functional`, run a script from a different directory that does `(execfile "hello.bbl")`, verify it resolves via `BBL_PATH`

### 6.5 final functional tests

| test file | validates |
|-----------|-----------|
| `sandbox_reject_absolute.bbl` | absolute path in execfile → error |
| `sandbox_reject_parent.bbl` | `..` in execfile → error |
| `sandbox_chain.bbl` | chained execfile narrows sandbox |
| `args.bbl` | script arguments accessible via `args` table |
| `repl_basic.bbl` | (tested via expect-style or echo pipe to bbl) |
| `multifile.bbl` | execfile loads another script, returns value |
| `serialization.bbl` | full workflow: struct registration, vector of structs, binary data, read back from C++ |

### validation

- All unit + functional tests pass
- `bbl -e '(print "hello\n")'` works
- `bbl` with no args enters REPL, evaluates expressions, exits on Ctrl-D
- Sandbox violations produce clear error messages
- `tests/run_functional.sh` reports all green

---

## phase summary

| phase | deliverable | key tests |
|-------|-------------|-----------|
| 1 | lexer + parser + core eval (=/if/loop/fn/exec, arithmetic, closures) | unit: tokenizer, parser, eval via `bbl.exec()` |
| 2 | C functions + print + C++ setters/introspection + CLI + execfile + errors | unit: defn, push/arg API, setters, print capture; functional: hello.bbl |
| 3 | structs + vectors + method dispatch + type descriptors (vector/string/binary) | unit: StructBuilder, getVector, `.` dispatch; functional: structs.bbl |
| 4 | tables + strings + table type descriptor | unit: table methods, interning, closure GC capture; functional: tables.bbl |
| 5 | binary data + GC + userdata/TypeBuilder + full stdlib (file I/O, math) | unit: GC stress, filebytes sandbox, TypeBuilder; functional: binary/math |
| 6 | REPL + script args + sandbox + BBL_PATH + end-to-end | functional: sandbox, serialization workflow, BBL_PATH |

## file layout

```
bbl/
├── bbl.h                  # public API header
├── bbl.cpp                # implementation
├── main.cpp               # bbl CLI entry point
├── CMakeLists.txt         # build (library + cli + tests)
├── tests/
│   ├── test_bbl.cpp       # unit tests
│   ├── run_functional.sh  # functional test runner
│   └── functional/        # .bbl test scripts
│       ├── hello.bbl
│       ├── arithmetic.bbl
│       └── ...
├── doc/                   # design documents
└── plan.md                # current work items
```
