# BBL Language Guide

BBL is a Lisp-like scripting language for embedding in C++ applications.  This document covers all syntax, keywords, types, and built-in functions.

---

## Syntax

Everything is an **s-expression**: `(operator args...)`.

```bbl
(+ 1 2)                   // 3
(print "hello world")     // prints: hello world
```

A script is a sequence of top-level s-expressions evaluated in order.

### Comments

Line comments only — `//` to end of line.

```bbl
(= x 10)  // this is a comment
// this entire line is a comment
```

---

## Data Types

| Type | Literal | Notes |
|------|---------|-------|
| `Int` | `42`, `-7` | 64-bit signed integer |
| `Float` | `3.14`, `-0.5` | 64-bit double |
| `Bool` | `true`, `false` | keywords |
| `Null` | `null` | keyword |
| `String` | `"hello"` | interned, supports `\"`, `\\`, `\n`, `\t` |
| `Binary` | `0b5:hello` | raw byte blob — `0b<size>:<bytes>` |
| `Fn` | `(fn (x) body)` | closures and C functions |
| `Vector` | `(vector int 1 2 3)` | typed homogeneous contiguous array |
| `Table` | `(table "k" "v")` | ordered key-value map |
| `Struct` | C++-registered | fixed-layout fields, value semantics |
| `UserData` | C++-registered | opaque C++ objects with methods |

---

## Assignment: `=`

`=` is the only assignment operator.  It uses **assign-or-create** semantics: if the name exists in scope, it rebinds it; otherwise, it creates a new binding.

```bbl
(= x 10)       // creates x = 10
(= x 20)       // rebinds x to 20
(= y (+ x 1))  // creates y = 21
```

Place expressions write to struct fields or table keys:

```bbl
(= player.hp 80)           // write to table key or struct field
```

---

## Arithmetic

All arithmetic operators take two or more operands.  Int and float mix freely (int is promoted to float).

| Op | Description | Example |
|----|-------------|---------|
| `+` | add (also string concat) | `(+ 1 2)` → `3` |
| `-` | subtract | `(- 10 3)` → `7` |
| `*` | multiply | `(* 3 4)` → `12` |
| `/` | divide (int division truncates) | `(/ 10 3)` → `3` |
| `%` | modulo (int only) | `(% 10 3)` → `1` |

String concatenation — when the left operand is a string, non-string operands are auto-coerced:

```bbl
(= s (+ "hello" " " "world"))     // "hello world"
(= s (+ "score=" 42))             // "score=42"
(= s (+ "pi=" 3.14 " ok=" true)) // "pi=3.14 ok=true"
```

---

## Bitwise

Integer-only bitwise operators.  Float operands are a type error.

| Op | Args | Description |
|----|------|-------------|
| `band` | 2+ | bitwise AND |
| `bor`  | 2+ | bitwise OR |
| `bxor` | 2+ | bitwise XOR |
| `bnot` | 1   | bitwise NOT (complement) |
| `shl`  | 2   | shift left |
| `shr`  | 2   | arithmetic shift right |

`band`, `bor`, `bxor` are variadic — they fold left-to-right over 2 or more arguments.

```bbl
(band 255 15)       // 15
(bor 1 2 4)         // 7
(bxor 255 15)       // 240
(bnot 0)            // -1
(shl 1 8)           // 256
(shr 256 4)         // 16

// combine flags
(= flags (bor 1 4))
(if (!= (band flags 1) 0)
    (print "flag 0 set\n"))
```

Shift amount must be non-negative.  Shifting by ≥ 64 returns 0 for `shl`, and -1 or 0 for `shr` depending on the sign of the left operand (arithmetic shift).

---

## Comparison

All comparisons return a `Bool`.

| Op | Description |
|----|-------------|
| `==` | equal |
| `!=` | not equal |
| `<` | less than |
| `>` | greater than |
| `<=` | less than or equal |
| `>=` | greater than or equal |

```bbl
(== 1 1)   // true
(< 3 5)    // true
(!= 1 2)   // true
```

---

## Logic

| Op | Description |
|----|-------------|
| `and` | short-circuit AND — returns first falsy or last value |
| `or` | short-circuit OR — returns first truthy or last value |
| `not` | logical negation |

```bbl
(and true false)   // false
(or false true)    // true
(not true)         // false

// short-circuit: second arg not evaluated
(= x 0)
(or true (= x 1))   // x stays 0
(and false (= x 1))  // x stays 0
```

---

## Control Flow

### do

```bbl
(do expr1 expr2 ... exprN)
```

Evaluates each expression sequentially and returns the value of the last one.  Runs in the enclosing scope.  Use `do` to group multiple expressions where one is expected (e.g. `if` branches):

```bbl
(= result (do (= a 10) (= b 20) (+ a b)))   // result = 30
```

### if

```bbl
(if condition then-expr)
(if condition then-expr else-expr)
```

Condition must be a `Bool`.  Each branch is a single expression — use `do` to group multiple:

```bbl
(if (> hp 0)
    (do (= alive true) (print "alive\n"))
    (do (= alive false) (print "dead\n")))
```

### loop

```bbl
(loop condition body...)
```

Repeats body while condition is `true`.  Condition must be a `Bool`.  `loop` is a statement (returns `null`).

```bbl
(= i 0)
(= sum 0)
(loop (< i 10)
    (= sum (+ sum i))
    (= i (+ i 1)))
// sum = 45, i = 10
```

### each

```bbl
(each index-var container body...)
```

Iterates over a vector or table, binding `index-var` to 0, 1, ..., length-1.  Access elements via `(container:at index-var)`.  The container is evaluated once.  `each` is a statement (returns `null`).

After the loop completes, the index variable remains in scope with value = container length.

```bbl
(= v (vector int 10 20 30))
(each i v
    (print (v:at i) "\n"))
// prints: 10\n20\n30\n — i is now 3
```

Works on tables too:

```bbl
(= t (table))
(t:push "a") (t:push "b") (t:push "c")
(each i t
    (print (t:at i) " "))
// prints: a b c
```

`each` replaces the common 3-line boilerplate of `(= i 0)` / `(loop (< i len) ...)` / `(= i (+ i 1))`.

For non-standard iteration patterns (e.g., starting at 1, iterating to `len - 2`, custom step), use `loop` instead.

### with

Scoped resource cleanup.  Binds a userdata value and runs the body.  The type's destructor runs when the body exits (normally or via exception).

```bbl
(with f (fopen "data.txt" "w")
    (f:write "hello"))
// f is closed here — destructor runs automatically
```

`with` creates a new scope — the binding is not visible after the block.  Returns the last body expression.

Nested `with` blocks clean up in LIFO order (inner first):

```bbl
(with src (fopen "in.txt" "r")
    (with dst (fopen "out.txt" "w")
        (dst:write (src:read))))
// dst closed, then src closed
```

Only userdata values are accepted.  Non-userdata produces a runtime error.

---

## Functions

### Defining functions

Functions are first-class values created with `fn`:

```bbl
(= double (fn (x) (* x 2)))
(double 5)   // 10

(= add (fn (x y) (+ x y)))
(add 3 4)    // 7
```

Multiple body expressions — the last expression is the return value:

```bbl
(= f (fn (x)
    (= y (* x 2))
    (+ y 1)))
(f 5)   // 11
```

### Closures

Functions capture free variables by value at definition time:

```bbl
(= make-adder (fn (n) (fn (x) (+ x n))))
(= add5 (make-adder 5))
(add5 3)   // 8
```

Value types (int, float, bool, struct) are snapshotted.  GC-managed types (table, vector, string, fn) are shared references — mutations through captured containers are visible outside.

### Recursion

Functions that reference their own name are automatically self-captured:

```bbl
(= factorial (fn (n)
    (= result 1)
    (if (<= n 1)
        (= result 1)
        (= result (* n (factorial (- n 1)))))
    result))
(factorial 5)   // 120
```

---

## Tables

Ordered key-value maps.  Keys can be any value type (strings and ints are most common).

```bbl
(= player (table "name" "hero" "hp" 100 "alive" true))
(print player.name)   // "hero"  (dot access looks up string key)
(= player.hp 80)      // write to string key via dot
```

Integer dot syntax reads/writes integer keys:

```bbl
(= items (table))
(items:push "sword")
(print items.0)        // "sword" — integer dot
(= items.0 "axe")     // write via integer dot
```

### Constructor

```bbl
(table key1 val1 key2 val2 ...)
```

### Methods

| Method | Description | Example |
|--------|-------------|---------|
| `length` | number of entries | `(player:length)` |
| `get` | get value by key | `(player:get "hp")` |
| `set` | set key-value pair | `(player:set "hp" 90)` |
| `delete` | remove a key | `(player:delete "alive")` |
| `has` | check if key exists | `(player:has "name")` → `true` |
| `keys` | table of all keys | `(player:keys)` |
| `push` | append with auto-int key (0-based) | `(t:push "item")` |
| `pop` | remove & return last int-keyed entry | `(t:pop)` |
| `at` | access by 0-based position among int keys | `(t:at 0)` |

Dot field reads on a table do automatic string-key lookup:

```bbl
player.name     // equivalent to (player:get "name")
```

---

## Vectors

Typed, homogeneous contiguous arrays.  The type is specified at creation.

```bbl
(= v (vector int 10 20 30))
(print (v:length))   // 3
(v:push 40)
(print (v:at 0))     // 10
(print v.0)          // 10 — integer dot syntax
(= last (v:pop))     // 40
```

### Constructor

```bbl
(vector type-name values...)
```

Types: `int`, `float`, `bool`, or any registered struct name.

### Methods

| Method | Description |
|--------|-------------|
| `length` | number of elements |
| `push` | append an element |
| `pop` | remove & return last element |
| `clear` | remove all elements |
| `at` | read by 0-based index |
| `set` | write by 0-based index — `(v:set 0 newval)` |

For struct vectors, elements have full dot access:

```bbl
(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))
(print (verts:at 0).x)
(print verts.0.x)          // same — integer dot chains with field access
(= verts.0 (vertex 5 5 5)) // integer dot write
```

---

## Strings

Strings are interned and immutable.  GC-managed — unreachable strings are swept during garbage collection, keeping the intern table clean.  Concatenate with `+`.

```bbl
(= s (+ "hello" " " "world"))
(print s:length)   // 11
```

### Access forms

No-arg methods work as both dot-access properties and call forms.  Methods with arguments require the call form.

```bbl
// dot-access — no-arg only
(print "hello".length)       // 5
(print "hello".upper)        // HELLO

// call form — required for methods with arguments
(print ("hello".at 0))       // h
(print ("hello world".slice 0 5))   // hello
```

### Methods

#### length

Returns the byte length of the string.

```bbl
(print "hello".length)   // 5
(print "".length)         // 0
```

#### at

`(s:at index)` — returns a single-character string at the given byte index.  Throws on out-of-range (negative or >= length).

```bbl
(print ("hello".at 0))   // h
(print ("hello".at 4))   // o
("hello".at 5)            // throws: index 5 out of bounds (length 5)
("hello".at -1)           // throws: index -1 out of bounds
```

#### slice

`(s:slice start [end])` — returns a substring from `start` (inclusive) to `end` (exclusive).  If `end` is omitted, slices to the end of the string.  Both indices are clamped to `[0, length]` — no out-of-bounds errors.

```bbl
(print ("hello world".slice 0 5))   // hello
(print ("hello world".slice 6))     // world
(print ("hello".slice 0 100))       // hello  (end clamped)
(print ("hello".slice 3 1))         // ""     (start >= end → empty)
```

#### find

`(s:find needle [start])` — returns the byte index of the first occurrence of `needle`, or `-1` if not found.  Optional `start` sets the search origin (must be >= 0).

```bbl
(print ("hello world".find "world"))    // 6
(print ("hello".find "xyz"))            // -1
(print ("hello".find "l" 3))            // 3
(print ("hello".find "l" 4))            // -1
("hello".find "l" -1)                   // throws: start must be >= 0
```

#### contains

`(s:contains needle)` — returns `true` if `needle` is found anywhere in the string.

```bbl
(print ("hello world".contains "world"))   // true
(print ("hello".contains "xyz"))           // false
```

#### starts-with

`(s:starts-with prefix)` — returns `true` if the string begins with `prefix`.

```bbl
(print ("hello".starts-with "hel"))   // true
(print ("hello".starts-with "world")) // false
```

#### ends-with

`(s:ends-with suffix)` — returns `true` if the string ends with `suffix`.

```bbl
(print ("hello".ends-with "llo"))     // true
(print ("hello".ends-with "hel"))     // false
```

#### upper

Returns a new string with all ASCII characters converted to uppercase.

```bbl
(print "hello".upper)          // HELLO
(print ("hello".upper))        // HELLO  (call form)
```

#### lower

Returns a new string with all ASCII characters converted to lowercase.

```bbl
(print "HELLO".lower)          // hello
```

#### trim

Returns a new string with leading and trailing whitespace removed.  Whitespace characters: space, tab, newline, carriage return, form feed, vertical tab.

```bbl
(print ("  hi  ".trim))        // hi
(print ("\t\nhello\n".trim))   // hello
```

#### trim-left

Returns a new string with leading whitespace removed (trailing whitespace preserved).

```bbl
(print ("  hi  ".trim-left))   // "hi  "
```

#### trim-right

Returns a new string with trailing whitespace removed (leading whitespace preserved).

```bbl
(print ("  hi  ".trim-right))  // "  hi"
```

#### replace

`(s:replace old new)` — returns a new string with all occurrences of `old` replaced by `new`.  Throws on empty search string.

```bbl
(print ("aXbXc".replace "X" "-"))     // a-b-c
(print ("hello".replace "l" "L"))     // heLLo
("abc".replace "" "x")                // throws: search string must not be empty
```

#### split

`(s:split separator)` — splits the string on `separator` and returns a table with integer keys `0, 1, 2, ...`.  Throws on empty separator.

```bbl
(= parts ("a,b,c".split ","))
(print parts.0)    // a
(print parts.1)    // b
(print parts.2)    // c

("abc".split "")   // throws: separator must not be empty
```

#### join

`(separator:join container)` — joins the elements of a table or vector into a single string, separated by the receiver string.  Table elements are joined in integer-key order (`0, 1, ...`).

```bbl
(= t (table))
(t:push "x") (t:push "y") (t:push "z")
(print (",".join t))       // x,y,z
(print (" - ".join t))     // x - y - z

(= v (vector int 1 2 3))
(print (",".join v))       // 1,2,3
```

#### pad-left

`(s:pad-left width [fill])` — left-pads the string to `width` characters.  `fill` defaults to a space.  If the string is already >= `width`, returns unchanged.  Fill must be a single-character string.

```bbl
(print ((str 42):pad-left 6))        // "    42"
(print ((str 42):pad-left 6 "0"))    // "000042"
(print ("hello".pad-left 3))         // "hello"  (already >= width)
```

#### pad-right

`(s:pad-right width [fill])` — right-pads the string to `width` characters.  `fill` defaults to a space.  If the string is already >= `width`, returns unchanged.  Fill must be a single-character string.

```bbl
(print ((str 42):pad-right 6))       // "42    "
(print ((str 42):pad-right 6 "."))   // "42...."
```

---

## Binary Data

Raw byte blobs, useful for file I/O and binary protocols.

```bbl
(= b 0b5:hello)
(print b:length)   // 5
```

### Methods

| Method | Description |
|--------|-------------|
| `length` | byte count |

---

## Structs

Structs are defined from C++ and used in scripts.  They have fixed-layout fields and value semantics (copied on assignment).

```bbl
// assuming 'vertex' with fields x, y, z is registered from C++
(= v (vertex 1.0 2.0 3.0))
(print v.x)        // 1.0
(= v.x 5.0)        // write field
```

---

## exec / execfile

### exec

Evaluate a string as BBL code.  Runs in an isolated scope.

```bbl
(= result (exec "(+ 1 2)"))   // result = 3
```

### execfile

Execute a `.bbl` file.  The file runs in the current scope.

```bbl
(execfile "setup.bbl")
```

**Path sandboxing**: by default, `execfile` and `filebytes` only allow relative paths without `..`.  The C++ host can set `bbl.allowOpenFilesystem = true` to allow absolute paths and parent traversal.  The `bbl` binary interpreter enables this by default.

---

## Standard Library

Available when the host program calls `BBL::addStdLib(bbl)`.

### print

Variadic — prints all arguments to stdout with no separator and no trailing newline.

```bbl
(print "x = " x "\n")
```

### str

Converts any value to its string representation.  Uses the same formatting as `print`.

```bbl
(= s (str 42))      // "42"
(= s (str 3.14))    // "3.14"
(= s (str true))    // "true"
(= s (str null))    // "null"
```

### typeof

Returns the type name of any value as a string.  Takes exactly one argument.

```bbl
(typeof 42)          // "int"
(typeof 3.14)        // "float"
(typeof "hello")     // "string"
(typeof true)        // "bool"
(typeof null)        // "null"
(typeof sqrt)        // "fn"
(typeof (table))     // "table"
(typeof (vector int)) // "vector"
```

Type name strings: `"int"`, `"float"`, `"string"`, `"bool"`, `"null"`, `"binary"`, `"fn"`, `"vector"`, `"table"`, `"struct"`, `"userdata"`.  Both BBL and C functions return `"fn"`.

### int

Parse a string to an integer, or convert a float (truncates toward zero).  Throws on invalid input, partial parse, or overflow.

```bbl
(= n (int "42"))     // 42
(= n (int "-7"))     // -7
(= n (int 3.9))     // 3
(= n (int -2.7))    // -2
```

### float

Parse a string to a float, or convert an integer.  Throws on invalid input, partial parse, or overflow.

```bbl
(= f (float "3.14"))  // 3.14
(= f (float 42))      // 42.0
```

### fmt

Format string with `{}` placeholders.  Each `{}` consumes the next argument.  Use `{{` / `}}` for literal braces.  Throws on argument count mismatch or lone braces.

```bbl
(= s (fmt "{} + {} = {}" 1 2 3))   // "1 + 2 = 3"
(= s (fmt "use {{}} for placeholders"))  // "use {} for placeholders"
```

### Math functions

| Function | Args | Description |
|----------|------|-------------|
| `sin` | 1 | sine (radians) |
| `cos` | 1 | cosine |
| `tan` | 1 | tangent |
| `asin` | 1 | arcsine |
| `acos` | 1 | arccosine |
| `atan` | 1 | arctangent |
| `atan2` | 2 | two-argument arctangent |
| `sqrt` | 1 | square root (errors on negative) |
| `abs` | 1 | absolute value |
| `floor` | 1 | floor |
| `ceil` | 1 | ceiling |
| `min` | 2 | minimum |
| `max` | 2 | maximum |
| `pow` | 2 | power |
| `log` | 1 | natural logarithm |
| `log2` | 1 | base-2 logarithm |
| `log10` | 1 | base-10 logarithm |
| `exp` | 1 | e^x |

Constants: `pi` (3.14159…), `e` (2.71828…)

### File I/O

| Function | Description |
|----------|-------------|
| `fopen` | `(fopen path [mode])` — open file, returns `File` userdata |
| `filebytes` | `(filebytes path)` — read file into a `Binary` |

#### File methods

| Method | Description |
|--------|-------------|
| `read` | read entire file as string |
| `read-line` | read one line (strips trailing newline), null at EOF |
| `read-bytes` | `(f:read-bytes n)` — read `n` bytes as `Binary` |
| `write` | `(f:write str)` — write string |
| `write-bytes` | `(f:write-bytes bin)` — write `Binary` data |
| `close` | close the file handle |
| `flush` | flush the file handle |

```bbl
(= f (fopen "data.txt" "w"))
(f:write "hello world")
(f:close)

(= f2 (fopen "data.txt" "r"))
(= contents (f2:read))
(f2:close)
(print contents)   // "hello world"
```

#### Standard Streams

Three pre-opened File globals: `stdin`, `stdout`, `stderr`.  They support all File methods.  `close` on a standard stream is a safe no-op.

```bbl
(= line (stdin:read-line))
(stdout:write "hello\n")
(stderr:write "error\n")
```

---

## UserData (C++ types)

UserData types are registered from C++ and exposed in scripts.  They behave like objects with methods.  See [cpp-api.md](features/cpp-api.md) for the C++ registration API.

---

## Scoping Rules

- **Root scope**: the outermost scope of a script.
- **Function scope**: each `fn` call creates a fresh scope.  Parameters are bound in this scope.
- **Shared scope**: `if`, `loop`, `each`, and body expressions run in the enclosing scope (no new scope is created).
- **Captures**: free variables in a `fn` body are captured by value at `fn` evaluation time.  Value types are snapshotted; GC types are shared references.

---

## Complete Keyword Reference

| Keyword | Category | Description |
|---------|----------|-------------|
| `=` | assignment | assign-or-create a variable; place write on struct/table fields |
| `if` | control | conditional — `(if cond then [else])` |
| `loop` | control | while-loop — `(loop cond body...)` |
| `each` | control | index iteration — `(each i container body...)` |
| `fn` | function | anonymous function — `(fn (params...) body...)` |
| `and` | logic | short-circuit AND |
| `or` | logic | short-circuit OR |
| `not` | logic | logical negation |
| `exec` | eval | evaluate string as code |
| `execfile` | eval | execute a `.bbl` file |
| `vector` | data | create typed vector — `(vector type vals...)` |
| `table` | data | create table — `(table k v k v ...)` |
| `+` | arithmetic | add / string concat |
| `-` | arithmetic | subtract |
| `*` | arithmetic | multiply |
| `/` | arithmetic | divide |
| `%` | arithmetic | modulo |
| `==` | comparison | equal |
| `!=` | comparison | not equal |
| `<` | comparison | less than |
| `>` | comparison | greater than |
| `<=` | comparison | less or equal |
| `>=` | comparison | greater or equal |
| `true` | literal | boolean true |
| `false` | literal | boolean false |
| `null` | literal | null value |
| `with` | resource | scoped resource cleanup — `(with name init body...)` |

---

## Complete Example

```bbl
// Fibonacci with memoization using a table
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
(= i 0)
(loop (< i 20)
    (results:push (fib i))
    (= i (+ i 1)))

(each i results
    (print "fib(" i ") = " (results:at i) "\n"))
```
