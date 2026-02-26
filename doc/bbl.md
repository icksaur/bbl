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

### Constructor

```bbl
(table key1 val1 key2 val2 ...)
```

### Methods

| Method | Description | Example |
|--------|-------------|---------|
| `length` | number of entries | `(player.length)` |
| `get` | get value by key | `(player.get "hp")` |
| `set` | set key-value pair | `(player.set "hp" 90)` |
| `delete` | remove a key | `(player.delete "alive")` |
| `has` | check if key exists | `(player.has "name")` → `true` |
| `keys` | table of all keys | `(player.keys)` |
| `push` | append with auto-int key | `(t.push "item")` |
| `pop` | remove & return last int-keyed entry | `(t.pop)` |
| `at` | access by 0-based position among int keys | `(t.at 0)` |

Dot field reads on a table do automatic string-key lookup:

```bbl
player.name     // equivalent to (player.get "name")
```

---

## Vectors

Typed, homogeneous contiguous arrays.  The type is specified at creation.

```bbl
(= v (vector int 10 20 30))
(print (v.length))   // 3
(v.push 40)
(print (v.at 0))     // 10
(= last (v.pop))     // 40
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
| `at` | access by 0-based index |

For struct vectors, elements have full dot access:

```bbl
(= verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))
(print (verts.at 0).x)
```

---

## Strings

Strings are interned and immutable.  Concatenate with `+`.

```bbl
(= s (+ "hello" " " "world"))
(print s.length)   // 11
```

### Methods

| Method | Description |
|--------|-------------|
| `length` | byte length of string |

### Properties

Dot access on a string for `length` returns the byte length.  No other methods are available.

---

## Binary Data

Raw byte blobs, useful for file I/O and binary protocols.

```bbl
(= b 0b5:hello)
(print b.length)   // 5
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
| `read-bytes` | `(f.read-bytes n)` — read `n` bytes as `Binary` |
| `write` | `(f.write str)` — write string |
| `write-bytes` | `(f.write-bytes bin)` — write `Binary` data |
| `close` | close the file handle |
| `flush` | flush the file handle |

```bbl
(= f (fopen "data.txt" "w"))
(f.write "hello world")
(f.close)

(= f2 (fopen "data.txt" "r"))
(= contents (f2.read))
(f2.close)
(print contents)   // "hello world"
```

---

## UserData (C++ types)

UserData types are registered from C++ and exposed in scripts.  They behave like objects with methods.  See [cpp-api.md](features/cpp-api.md) for the C++ registration API.

---

## Scoping Rules

- **Root scope**: the outermost scope of a script.
- **Function scope**: each `fn` call creates a fresh scope.  Parameters are bound in this scope.
- **Shared scope**: `if`, `loop`, and body expressions run in the enclosing scope (no new scope is created).
- **Captures**: free variables in a `fn` body are captured by value at `fn` evaluation time.  Value types are snapshotted; GC types are shared references.

---

## Complete Keyword Reference

| Keyword | Category | Description |
|---------|----------|-------------|
| `=` | assignment | assign-or-create a variable; place write on struct/table fields |
| `if` | control | conditional — `(if cond then [else])` |
| `loop` | control | while-loop — `(loop cond body...)` |
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

---

## Complete Example

```bbl
// Fibonacci with memoization using a table
(= memo (table))
(= fib (fn (n)
    (= result 0)
    (if (memo.has n)
        (= result (memo.get n))
        (if (<= n 1)
            (= result n)
            (= result (+ (fib (- n 1)) (fib (- n 2))))))
    (memo.set n result)
    result))

(= i 0)
(loop (< i 20)
    (print "fib(" i ") = " (fib i))
    (= i (+ i 1)))
```
