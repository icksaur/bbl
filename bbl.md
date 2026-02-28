# BBL Language Reference

BBL (Basic Binary Lisp) is a scripting language for serializing C++ data structures
and binary blobs.  Prefix s-expression syntax.

---

## Syntax

All code is written as s-expressions: `(operator args...)`.

```bbl
(+ 1 2)                     // arithmetic
(= x 10)                    // assignment
(print "hello\n")           // function call
```

Comments start with `//` and run to end of line.

---

## Data Types

### Value Types

Copied on assignment.  No heap allocation, no GC involvement.

| Type     | Literal                | Notes                                      |
|----------|------------------------|--------------------------------------------|
| `int`    | `42`, `-7`             | 64-bit signed integer                      |
| `float`  | `3.14`, `-0.5`         | 64-bit IEEE double                         |
| `bool`   | `true`, `false`        | Keywords                                   |
| `null`   | `null`                 | Keyword                                    |
| `struct` | `(vertex 1.0 2.0 3.0)` | C++-registered, POD binary layout          |

### GC-Managed Types

Shared on assignment.  Managed by mark-and-sweep garbage collector.

| Type       | Literal / Construction                       | Notes                                       |
|------------|----------------------------------------------|---------------------------------------------|
| `string`   | `"hello"`                                    | Interned, immutable, UTF-8                  |
| `binary`   | `0b5:hello`                                  | Raw byte buffer                             |
| `fn`       | `(fn (x) (* x 2))`                          | Function / closure                          |
| `vector`   | `(vector int 10 20 30)`                      | Contiguous typed storage                    |
| `table`    | `(table "name" "hero" "hp" 100)`             | Heterogeneous key-value container           |
| `userdata` | C++ only                                     | Opaque `void*` with type descriptor         |

---

## Literals

### Numbers

```bbl
42          // int
-7          // negative int
3.14        // float
-0.5        // negative float
```

Two numeric types only: `int` (int64) and `float` (double).  No suffixes, no hex
literals.  Struct fields may use narrow C types (`int32`, `float32`, etc.) but
script-visible values are always widened to int64 or double.

### Strings

```bbl
"hello"
"line one\nline two"
"tab\there"
"quote: \""
"backslash: \\"
```

Escape sequences: `\"`, `\\`, `\n`, `\t`.

All strings are interned -- duplicate content shares one allocation.  Equality
comparison is O(1) pointer compare.

### Binary

```bbl
0b5:hello
0b65536:<65536 raw bytes>
```

Format: `0b<size>:<raw bytes>`.  The lexer reads exactly `size` bytes verbatim
after the colon.  No escaping, no encoding.  Binary data is mutable in memory.

### Booleans and Null

```bbl
true
false
null
```

Keywords, not symbols.

---

## Assignment

```bbl
(= x 10)              // create x in current scope
(= x 20)              // rebind x (walks scope chain: local -> captured -> root)
(= player.hp 80)      // place expression: struct field or table key
(= v.0 99)            // place expression: vector/table integer index
```

Assign-or-create semantics: if the name exists anywhere in the scope chain
(local, captured, root), rebind it.  Otherwise create a new binding in the
current scope.

Only single-level place expressions are supported.  For deep mutation, use
an intermediate variable:

```bbl
(= tmp (verts:at 0))
(= tmp.x 5.0)
(verts:set 0 tmp)
```

---

## Operators

### Arithmetic

```bbl
(+ 1 2)          // 3
(- 10 3)         // 7
(* 4 5)          // 20
(/ 20 4)         // 5
(% 10 3)         // 1
```

Promotion: int+int=int, float+float=float, int+float=float.

String concatenation: when the left operand is a string, `+` is variadic and
auto-coerces non-string arguments:

```bbl
(+ "hello" " " "world")    // "hello world"
(+ "score=" 42)             // "score=42"
```

### Comparison

```bbl
(== 1 1)          // true
(!= 1 2)          // true
(< 1 2)           // true
(> 2 1)           // true
(<= 2 2)          // true
(>= 2 2)          // true
```

String `==`/`!=` use interned pointer equality (O(1)).  String ordering
(`<`, `>`, `<=`, `>=`) is lexicographic, byte-level.

### Logical

```bbl
(and true true)   // true
(or false true)   // true
(not true)        // false
```

`and` and `or` are short-circuit special forms.  All three require bool
operands -- no implicit truthiness.

### Bitwise

```bbl
(band 255 15)      // 15        AND (variadic)
(bor 1 2 4)        // 7         OR (variadic)
(bxor 255 15)      // 240       XOR (variadic)
(bnot 0)           // -1        NOT
(shl 1 8)          // 256       shift left
(shr 256 4)        // 16        arithmetic shift right
```

Integer-only.  Float operands are a type error.  `shr` propagates the sign bit.
Negative shift amounts throw.

---

## Control Flow

### do

Evaluate multiple expressions sequentially.  Returns the last value.

```bbl
(do
    (= a 10)
    (= b 20)
    (+ a b))       // 30
```

### if

```bbl
(if (== x 0) (print "zero"))
(if (> x 0) (print "positive") (print "non-positive"))
```

Returns the value of the taken branch.  When the else branch is absent and the
condition is false, returns null.  Condition must be bool (no implicit
truthiness).

As an expression:

```bbl
(= sign (if (> x 0) "positive" "non-positive"))
(= abs-x (if (< x 0) (- 0 x) x))
```

Use `do` for multi-statement branches:

```bbl
(= result (if (> x 0)
    (do (print "positive\n") x)
    (do (print "non-positive\n") 0)))
```

### loop

```bbl
(= i 0)
(loop (< i 10)
    (print i "\n")
    (= i (+ i 1)))
```

While-loop.  Condition must be bool.  Statement (returns null).

### each

```bbl
(= data (vector int 10 20 30))
(each i data
    (print "element " i " = " (data:at i) "\n"))
```

Index-only iteration from 0 to `length - 1`.  Container evaluated once.
Statement (returns null).  Works on vectors and tables.  After the loop,
the index variable remains bound (value = length).

### break / continue

```bbl
(= i 0)
(loop true
    (if (>= i 5) (break))
    (= i (+ i 1)))

(= i 0)
(loop (< i 10)
    (= i (+ i 1))
    (if (== (% i 2) 0) (continue))
    (print i "\n"))
```

`break` exits the innermost loop.  `continue` skips to the next iteration.
Both are errors outside of a loop.  Neither takes arguments.

---

## Functions

### Definition and Calling

```bbl
(= double (fn (x) (* x 2)))
(double 21)    // 42

(= greet (fn (name)
    (print "hello, " name "!\n")))
(greet "world")
```

Functions are first-class values.  Parameters are untyped.  Arity is enforced
at call time.  The return value is the last expression evaluated in the body.

### Closures

```bbl
(= make-adder (fn (n) (fn (x) (+ x n))))
(= add5 (make-adder 5))
(add5 3)    // 8
```

Free variables are captured when `fn` is evaluated:

- **Value types** (int, float, bool, null, struct): copied -- closure gets its
  own snapshot.  Changes to the original do not affect the closure.
- **GC-managed types** (string, binary, fn, vector, table, userdata): shared
  reference -- mutations are visible to all holders.

Rebinding an outer variable after capture does not affect the closure.

### Recursion

Direct recursion works -- a function's own name is auto-captured after
assignment:

```bbl
(= factorial (fn (n)
    (if (<= n 1)
        (= result 1)
        (= result (* n (factorial (- n 1)))))
    result))
```

For mutual recursion, use a table to hold both functions.

### Conditional Returns

Since `if` returns a value, use it directly:

```bbl
(= abs (fn (x)
    (if (< x 0) (- 0 x) x)))
```

For multi-branch chains, assign in each branch:

```bbl
(= label "other")
(if (== x 0) (= label "zero"))
(if (== x 1) (= label "one"))
```

---

## Tables

Heterogeneous key-value container with both string and integer keys.

### Construction

```bbl
(= player (table "name" "hero" "hp" 100 "alive" true))
(= items (table))
```

Arguments are key-value pairs: `(table key1 val1 key2 val2 ...)`.

### Access

```bbl
// dot notation for string keys
(print player.name)
(= player.hp 80)

// integer dot notation
(= items (table))
(items:push "sword")
(items:push "shield")
(print items.0)          // "sword"
(= items.0 "axe")
```

### Methods

| Method              | Description                                          |
|---------------------|------------------------------------------------------|
| `(t:length)`        | Number of entries                                    |
| `(t:get key)`       | Get value by key (string or int)                     |
| `(t:set key val)`   | Set key-value pair                                   |
| `(t:delete key)`    | Remove entry by key                                  |
| `(t:has key)`       | Check if key exists (returns bool)                   |
| `(t:keys)`          | Return a table of all keys                           |
| `(t:push val)`      | Append with auto-incrementing integer key (0-based)  |
| `(t:pop)`           | Remove and return last entry                         |
| `(t:at i)`          | Get value at position `i` (0-based, insertion order) |

### Self-Passing Sugar

When a table key holds a function, colon syntax passes the table as the first
argument automatically:

```bbl
(= obj (table
    "name" "bbl"
    "greet" (fn (self)
        (+ "hello from " (self:get "name")))))

(obj:greet)    // "hello from bbl"
               // equivalent to: ((obj.greet) obj)
```

Resolution order for `(t:key)`:
1. Built-in table method? -- call it
2. Table string key lookup -- if value is a function, call with table prepended

---

## Vectors

Contiguous typed storage, identical layout to C++ `std::vector<T>`.

### Construction

```bbl
(= nums (vector int 10 20 30))
(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))
(= empty (vector float))
```

Type argument is required.  All elements must match the declared type.
Allowed element types: `int`, `float`, `bool`, registered structs.

### Access

```bbl
(print (nums:at 0))      // 10
(nums:set 1 99)
(print nums.0)            // integer dot notation
(= nums.0 42)
```

### Methods

| Method             | Description                                  |
|--------------------|----------------------------------------------|
| `(v:length)`       | Number of elements                           |
| `(v:push val)`     | Append element                               |
| `(v:pop)`          | Remove and return last element               |
| `(v:at i)`         | Get element at index                         |
| `(v:set i val)`    | Set element at index                         |
| `(v:clear)`        | Remove all elements                          |
| `(v:resize n)`     | Set length (zero-fills if growing)           |
| `(v:reserve n)`    | Pre-allocate capacity without changing length|

### C++ Zero-Copy Access

The primary value proposition.  From C++, `getVectorData<T>()` returns a raw
pointer to the contiguous element buffer -- usable directly for GPU upload,
serialization, or any C API expecting `T*`.

---

## Strings

Immutable, interned, GC-managed.

### Methods

| Method                      | Description                              |
|-----------------------------|------------------------------------------|
| `(s:length)`                | Byte length                              |
| `(s:at i)`                  | Single character at byte index           |
| `(s:slice start end)`       | Substring (clamps silently)              |
| `(s:find needle)`           | First index of needle, or -1             |
| `(s:contains needle)`       | Bool                                     |
| `(s:starts-with prefix)`    | Bool                                     |
| `(s:ends-with suffix)`      | Bool                                     |
| `(s:upper)`                 | Uppercase copy                           |
| `(s:lower)`                 | Lowercase copy                           |
| `(s:trim)`                  | Strip whitespace from both ends          |
| `(s:trim-left)`             | Strip leading whitespace                 |
| `(s:trim-right)`            | Strip trailing whitespace                |
| `(s:replace old new)`       | Replace all occurrences                  |
| `(s:split sep)`             | Split into table of substrings           |
| `(sep:join table)`          | Join table elements with separator       |
| `(s:pad-left width [fill])` | Left-pad to width (default fill: space)  |
| `(s:pad-right width [fill])`| Right-pad to width (default fill: space) |

All operations are byte-indexed.

### Concatenation

```bbl
(+ "hello" " " "world")     // variadic
(+ "score=" 42)              // auto-coerce right operands
```

### Comparison

```bbl
(== "a" "a")       // true  (pointer equality, O(1))
(< "a" "b")        // true  (lexicographic, byte-level)
```

---

## Binary Data

Raw byte buffers for textures, audio, mesh data, or any binary content.

### Literals

```bbl
(= b 0b5:hello)
(= texture 0b65536:<65536 raw bytes>)
```

### From File

```bbl
(= texture (filebytes "texture.png"))
```

### Methods

| Method         | Description       |
|----------------|-------------------|
| `(b:length)`   | Byte count        |

### Shell Generation

```bash
SIZE=$(stat -c%s texture.png)
printf '(= texture 0b%d:' "$SIZE" > scene.bbl
cat texture.png >> scene.bbl
printf ')\n' >> scene.bbl
```

---

## Structs

Value types with C++-compatible binary layout, registered exclusively from C++.
POD only -- fields must be numeric types, `bool`, or other registered structs.
No strings, containers, functions, or binaries in fields.

### Usage in Script

```bbl
// vertex registered from C++ via StructBuilder
(= v (vertex 1.0 2.0 3.0))    // positional args in registration order
(print v.x)                    // field read via dot
(= v.x 5.0)                   // field write via dot
```

### Nested Structs

```bbl
// triangle has fields a, b, c of type vertex
(= tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(print tri.a.x)                // chained reads allowed
```

Chained writes are NOT allowed -- use an intermediate variable.

### Properties

- Copy semantics (assignment copies bytes, no GC)
- C++-identical binary layout (sizeof, offsetof, alignment)
- No member functions (use free functions)
- Type descriptors are global on `BblState`, never freed during execution

---

## Userdata

Opaque C++ objects with registered method tables, created exclusively from C++.

### Usage in Script

```bbl
// File type registered by addFileIo()
(= f (fopen "data.txt" "w"))
(f:write "hello\n")
(f:close)
```

Methods accessed via colon syntax.  Dot access on userdata is an error.

### Deterministic Cleanup with `with`

```bbl
(with f (fopen "data.txt" "w")
    (f:write "hello\n")
    (f:flush))
// destructor runs here, even on error
```

`with` guarantees the destructor runs when the block exits (normal or
exception).  After `with`, the data pointer is nulled to prevent double-free.

---

## Method Resolution: Dot vs Colon

Two operators, one universal rule:

- **`.` (dot)** -- data access: struct fields, table string-key lookup, integer indices
- **`:` (colon)** -- method calls: built-in methods on any type, table self-passing

| Type       | Dot                  | Colon                                          |
|------------|----------------------|------------------------------------------------|
| `struct`   | fields (read/write)  | error (reserved)                               |
| `table`    | string-key lookup    | built-in methods, then self-passing sugar       |
| `vector`   | integer index        | `push`, `pop`, `length`, `at`, `set`, etc.     |
| `string`   | error                | all string methods                             |
| `binary`   | error                | `length`                                       |
| `userdata` | error                | registered methods                             |

Colon is only valid in call position (head of a list).  `v:length` outside a
call is a runtime error.  Integer-colon (`v:0`) is a parse error.

---

## Error Handling

### try/catch

```bbl
(= result (try
    (/ 1 0)
    (catch e e)))
// result = "division by zero"

(try
    (+ 1 2)
    (catch e "error"))
// no error -> returns 3
```

Last child of `try` must be `(catch symbol body...)`.  The error message
(a string) is bound to the error variable.  Only BBL runtime errors are caught.

### Error Conditions

Type mismatch, non-bool condition, out of bounds, undefined symbol, arity
mismatch, division by zero, file I/O failure, parse error, pop on empty
container, field access on wrong type.

Errors print a backtrace to stderr showing each call frame with file and line.

---

## exec / execfile

### exec

```bbl
(= result (exec "(+ 1 2)"))    // 3, fresh isolated scope
```

Evaluates a string as BBL code.  From script: creates a fresh scope, returns
last expression.  From C++: accumulates into existing root scope.

### execfile

```bbl
(execfile "setup.bbl")
```

Runs another `.bbl` file.  From script: fresh isolated scope, returns last
expression.  From C++: accumulates into root scope (second call sees first
call's definitions).

Path resolution is relative to the calling script's directory.

---

## Standard Library

Loaded from C++ via `BBL::addStdLib(bbl)`.  Individual modules available:
`addPrint()`, `addMath()`, `addFileIo()`, `addOs()`.

### Core Functions

| Function                | Description                                      |
|-------------------------|--------------------------------------------------|
| `(print args...)`       | Print to stdout, no trailing newline              |
| `(str val)`             | Convert any value to string                      |
| `(typeof val)`          | Type name as string ("int", "table", etc.)       |
| `(int val)`             | Parse string to int, or truncate float           |
| `(float val)`           | Parse string to float, or promote int            |
| `(fmt "{} + {}" 1 2)`  | Format string (`{{`/`}}` for literal braces)     |

### Math

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `abs`, `floor`,
`ceil`, `min`, `max`, `pow`, `log`, `log2`, `log10`, `exp`.

Constants: `pi`, `e`.

### File I/O

| Function / Method          | Description                              |
|----------------------------|------------------------------------------|
| `(fopen path [mode])`      | Open file, returns File userdata         |
| `(filebytes path)`         | Read entire file into binary             |
| `(f:read)`                 | Read file as string                      |
| `(f:read-line)`            | Read one line (strips newline), null at EOF |
| `(f:read-bytes n)`         | Read n bytes as binary                   |
| `(f:write str)`            | Write string                             |
| `(f:write-bytes blob)`     | Write binary data                        |
| `(f:close)`                | Close file                               |
| `(f:flush)`                | Flush buffers                            |

File modes: `"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, etc. (standard C fopen modes).

### Standard Streams

Three pre-opened File globals are available when `addFileIo()` (or `addStdLib()`) is called:

| Global   | Description                              |
|----------|------------------------------------------|
| `stdin`  | Standard input (File wrapping C stdin)   |
| `stdout` | Standard output (File wrapping C stdout) |
| `stderr` | Standard error (File wrapping C stderr)  |

They support all File methods.  `close` on a standard stream is a safe no-op.

```bbl
// line-by-line stdin processing
(= line (stdin:read-line))
(loop (!= line null)
    (print "line: " line "\n")
    (= line (stdin:read-line)))

// explicit output
(stdout:write "to stdout\n")
(stderr:write "error message\n")
```

### OS Library

Registered by `addOs()` (included in `addStdLib()`).  Provides operating-system
facilities for general-purpose shell scripting.

#### Environment

| Function                      | Description                                  |
|-------------------------------|----------------------------------------------|
| `(getenv name)`               | Read environment variable, null if unset     |
| `(setenv name value)`         | Set environment variable                     |
| `(unsetenv name)`             | Remove environment variable                  |

#### Time

| Function                      | Description                                  |
|-------------------------------|----------------------------------------------|
| `(time)`                      | Current Unix epoch in seconds (int)          |
| `(clock)`                     | CPU time used in seconds (float)             |
| `(sleep seconds)`             | Sleep for `seconds` (float, sub-second OK)   |
| `(date [fmt [timestamp]])`    | Format time via strftime (default `"%Y-%m-%d %H:%M:%S"`) |
| `(difftime t2 t1)`            | Difference in seconds between timestamps (float) |

#### Filesystem

| Function                      | Description                                  |
|-------------------------------|----------------------------------------------|
| `(getcwd)`                    | Current working directory (string)           |
| `(chdir path)`                | Change directory, returns bool               |
| `(mkdir path)`                | Create directory, returns bool               |
| `(remove path)`               | Delete file or empty directory, returns bool |
| `(rename old new)`            | Rename/move file, returns bool               |
| `(tmpname)`                   | Create temp file, return its path (string)   |
| `(stat path)`                 | File metadata table (`size`, `mtime`, `is-dir`, `is-file`), null if missing |
| `(glob pattern)`              | Expand shell glob, returns table with 1-based int keys |

#### Process

| Function                      | Description                                  |
|-------------------------------|----------------------------------------------|
| `(execute command)`           | Run shell command, return exit status (int)  |
| `(getpid)`                    | Current process ID (int)                     |
| `(exit [code])`               | Terminate process (default code 0)           |
| `(spawn command)`             | Launch child process, return Process userdata |
| `(spawn-detached command)`    | Launch detached background process, return PID (int) |

#### Process Methods

| Method                        | Description                                  |
|-------------------------------|----------------------------------------------|
| `(p:read)`                    | Read all remaining stdout as string          |
| `(p:read-line)`               | Read one line (strips newline), null at EOF  |
| `(p:wait)`                    | Wait for exit, return exit status (int)      |

```bbl
// Capture output of a command
(= p (spawn "ls -la"))
(= output (p:read))
(= status (p:wait))
(print output)

// Fire-and-forget background process
(= pid (spawn-detached "long-running-server"))
(print "launched as pid " pid "\n")

// File metadata
(= info (stat "/tmp"))
(print "is directory: " info.is-dir "\n")
```

---

## CLI

```
bbl [options] [script [args...]]
```

| Mode                        | Description                              |
|-----------------------------|------------------------------------------|
| `bbl script.bbl`           | Run script                               |
| `bbl script.bbl arg1 arg2` | Run with arguments                       |
| `bbl`                      | Interactive REPL                         |
| `bbl -e "(+ 1 2)"`        | Evaluate expression                      |
| `bbl -v`                   | Version                                  |
| `bbl -h`                   | Help                                     |

### Shebang

BBL scripts can start with a `#!` line for direct execution:

```bbl
#!/usr/bin/env bbl
(print "hello from script\n")
```

```sh
chmod +x hello.bbl
./hello.bbl
```

The shebang line is silently skipped by the lexer.  Only recognized at position 0 — `#` anywhere else is a syntax error.

### Script Arguments

Available as `args` table:

```bbl
// bbl build.bbl output.obj --verbose
(print (args:at 0))    // "output.obj"
(print (args:at 1))    // "--verbose"
```

All argument values are strings.  Use `int` or `float` to convert.  Use
value-returning `if` with `has` for defaults:

```bbl
(= output (if (args:has 0) (args:at 0) "default.obj"))
(= count  (if (args:has 1) (int (args:at 1)) 10))
```

### REPL

Prompt: `> ` for new expression, `. ` for continuation (unbalanced parens).
Non-null results printed automatically.  Errors print to stderr and continue.
Exit with Ctrl-D or Ctrl-C.

### Environment

`BBL_PATH`: colon-separated directory list for `execfile` resolution.  Falls
back to script-relative paths.

---

## Scoping Rules

One scope type: a symbol-to-value table.

- **Fresh scope** (new table): `fn` calls, `execfile` from script, `exec` from script
- **Shared scope** (enclosing table): `do`, `loop`, `if`, `each`

"Root scope" is just the outermost scope of the current script -- same data
structure as a function's local scope.

### Capture Mechanics

When `fn` is evaluated, free variables from the enclosing scope are copied into
the function value's capture environment:

- Value types: snapshot (independent copy)
- GC types: shared reference (same object)
- Rebinding the outer name after capture has no effect on the closure

`(= name value)` inside a closure modifies the captured binding, not the
original scope (for value types this means the closure's copy; for GC types the
object itself is shared but the binding slot is local).

---

## Sandboxing

Default: zero filesystem access.  Capabilities are opt-in:

| Capability      | Access                                             |
|-----------------|----------------------------------------------------|
| No stdlib       | Pure computation only                              |
| `addPrint()`    | stdout only                                        |
| `addMath()`     | No filesystem                                      |
| `addFileIo()`   | `fopen`, `filebytes`, `stdin`/`stdout`/`stderr` globals |
| `addOs()`       | Process execution, filesystem mutation, env mutation, exit |
| `defn()`        | Whatever the host function exposes                 |

Path rules for `execfile` and `filebytes` from script:
- Relative paths only (absolute paths rejected)
- No `..` traversal
- Resolved relative to calling script's directory
- Each exec'd script's sandbox root narrows to its own directory

C++ host is unsandboxed by default.  Set `bbl.allowOpenFilesystem = true` to
also disable sandbox checks for script calls (enabled by default in the `bbl`
CLI binary).

`fopen` follows standard C semantics and is not sandboxed.

---

## Keyword Reference

| Keyword     | Category       | Description                                    |
|-------------|----------------|------------------------------------------------|
| `=`         | assignment     | Assign-or-create                               |
| `do`        | control flow   | Sequential evaluation, return last             |
| `if`        | control flow   | Conditional (statement)                        |
| `loop`      | control flow   | While-loop                                     |
| `each`      | control flow   | Index iteration over container                 |
| `break`     | control flow   | Exit innermost loop                            |
| `continue`  | control flow   | Skip to next iteration                         |
| `fn`        | function       | Define function / closure                      |
| `try`       | error handling | Try block with catch                           |
| `catch`     | error handling | Error handler (inside try)                     |
| `with`      | resource mgmt  | Scoped userdata with deterministic destructor  |
| `exec`      | evaluation     | Evaluate string as code                        |
| `execfile`  | evaluation     | Execute file                                   |
| `and`       | logic          | Short-circuit logical AND                      |
| `or`        | logic          | Short-circuit logical OR                       |
| `not`       | logic          | Logical negation                               |
| `band`      | bitwise        | Bitwise AND (variadic)                         |
| `bor`       | bitwise        | Bitwise OR (variadic)                          |
| `bxor`      | bitwise        | Bitwise XOR (variadic)                         |
| `bnot`      | bitwise        | Bitwise NOT                                    |
| `shl`       | bitwise        | Shift left                                     |
| `shr`       | bitwise        | Arithmetic shift right                         |
| `true`      | literal        | Boolean true                                   |
| `false`     | literal        | Boolean false                                  |
| `null`      | literal        | Null value                                     |

---

## Complete Example

Fibonacci with memoization, demonstrating tables, recursion, vectors, each, and
string formatting:

```bbl
(= memo (table))

(= fib (fn (n)
    (= result 0)
    (if (memo:has n)
        (= result (memo:get n))
        (if (<= n 1)
            (= result n)
            (= result (+ (fib (- n 1)) (fib (- n 2))))))
    (memo:set n result)
    result))

(= results (vector int))
(each i (vector int 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19)
    (results:push (fib i)))

(each i results
    (print (fmt "fib({}) = {}\n" i (results:at i))))
```
