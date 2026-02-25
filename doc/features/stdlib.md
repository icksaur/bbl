# Standard Library

## goal

Document the built-in functions and types that ship with BBL but are not part of the core language.  These are optional — a minimal embedding can skip them entirely and register only custom C functions.  None of these require coupling to the BBL library's internal data structures; they wrap POSIX / C standard library facilities behind typed userdata.

## loading

From C++:

```cpp
BblState bbl;
BBL::addStdLib(bbl);    // registers everything below
```

Or pick individual modules:

```cpp
BBL::addPrint(bbl);     // only print
BBL::addFileIo(bbl);    // fopen, filebytes
BBL::addMath(bbl);      // sin, cos, sqrt, etc.
BBL::addString(bbl);    // string methods (deferred — placeholder for v1)
```

Each loader is idempotent — calling it twice is a no-op.

---

## print

Registered by `BBL::addPrint(bbl)`.

```bbl
(print "hello " name " has " count " items\n")
```

Variadic.  Prints each argument to stdout with no separator.  No automatic newline — include `\n` explicitly.

| argument type | formatting |
|---------------|------------|
| string | verbatim |
| int | decimal |
| float | `%g`-style (no trailing zeros) |
| bool | `true` / `false` |
| null | `null` |
| binary | `<binary N bytes>` |
| fn | `<fn name>` or `<fn anonymous>` |
| vector | `<vector T length=N>` |
| table | `<table length=N>` |
| struct | `<struct TypeName>` |
| userdata | `<userdata TypeName>` |

Returns nothing (void).

---

## file I/O

Registered by `BBL::addFileIo(bbl)`.  Provides `fopen`, `filebytes`, and the `File` typed userdata.

### fopen

```bbl
(def f (fopen "path.txt"))            // read mode (default)
(def f (fopen "path.txt" "w"))        // write mode
(def f (fopen "path.txt" "a"))        // append mode
```

Opens a file and returns a `File` (typed userdata, GC-managed).  Mode strings follow C `fopen()` conventions: `"r"` (default if omitted), `"w"`, `"a"`, `"rb"`, `"wb"`, etc.

Runtime error if the file cannot be opened.

### File type

`File` is a typed userdata registered via `TypeBuilder`.  It has an optional destructor that closes the handle if still open when collected by the GC.  **Prefer explicit `close`** for deterministic resource management — the GC does not guarantee when (or if, before `~BblState`) it will collect an object.

| method | signature | description |
|--------|-----------|-------------|
| `read` | `(f.read)` | read entire remaining contents as a string |
| `read-bytes` | `(f.read-bytes n)` | read up to `n` bytes, return `binary` |
| `write` | `(f.write str)` | write a string to the file |
| `write-bytes` | `(f.write-bytes blob)` | write a binary blob to the file |
| `close` | `(f.close)` | flush and close immediately |
| `flush` | `(f.flush)` | flush buffered output without closing |

`read` returns a string.  `read-bytes` returns a `binary`.  Both return an empty value at EOF.

`write` accepts a string.  `write-bytes` accepts a `binary`.  Both error on a file opened in read mode.

```bbl
// read a whole file
(def f (fopen "data.txt"))
(def contents (f.read))
(f.close)

// write line by line
(def out (fopen "log.txt" "w"))
(out.write "hello\n")
(out.write "world\n")
(out.close)
```

### filebytes

```bbl
(def tex (filebytes "texture.png"))
```

Reads an entire file into memory and returns a `binary`.  Opens the file, reads all bytes, closes the file, returns the result.  See [binary-data.md](binary-data.md) for the `BblBinary` type.

**Sandboxing**: from script, `filebytes` can only access files in the calling script's directory or child directories.  Absolute paths and paths containing `..` are a runtime error.  See [security.md](security.md).

Runtime error if the file cannot be opened or read.

---

## math

Registered by `BBL::addMath(bbl)`.  All math functions operate on `float` (f64) and return `float`.  Integer arguments are promoted to `float`.

| function | signature | description |
|----------|-----------|-------------|
| `sin` | `(sin x)` | sine (radians) |
| `cos` | `(cos x)` | cosine (radians) |
| `tan` | `(tan x)` | tangent (radians) |
| `asin` | `(asin x)` | arc sine |
| `acos` | `(acos x)` | arc cosine |
| `atan` | `(atan x)` | arc tangent |
| `atan2` | `(atan2 y x)` | two-argument arc tangent |
| `sqrt` | `(sqrt x)` | square root |
| `abs` | `(abs x)` | absolute value |
| `floor` | `(floor x)` | round toward −∞ |
| `ceil` | `(ceil x)` | round toward +∞ |
| `min` | `(min a b)` | smaller of two values |
| `max` | `(max a b)` | larger of two values |
| `pow` | `(pow base exp)` | exponentiation |
| `log` | `(log x)` | natural logarithm |
| `log2` | `(log2 x)` | base-2 logarithm |
| `log10` | `(log10 x)` | base-10 logarithm |
| `exp` | `(exp x)` | e^x |

`sqrt` of a negative number is a runtime error (not NaN — BBL does not expose IEEE special values).

### constants

`pi` and `e` are defined as `float` variables in the root scope by `addMath`:

```bbl
(def circumference (* 2.0 pi r))
```

---

## string methods

Registered by `BBL::addString(bbl)`.  **Deferred for v1** — see [backlog](../backlog.md) under "string operations".

Planned methods: `substring`, `index-of`, `replace`, `split`, `join`, `upper`, `lower`, `trim`, `to-int`, `to-float`, `format`.

---

## open questions

None.
