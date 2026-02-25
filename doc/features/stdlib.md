# standard library

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
BBL::addSocket(bbl);    // connect, listen
BBL::addString(bbl);    // string methods (deferred — placeholder for v1)
```

Each loader is idempotent — calling it twice is a no-op.

---

## print

Registered by `BBL::addPrint(bbl)`.

```bbl
[print "hello " name " has " count " items"]
```

Variadic.  Prints each argument to stdout with no separator.  Adds a trailing newline.

| argument type | formatting |
|---------------|------------|
| string | verbatim |
| int8–int64, uint8–uint64 | decimal |
| f32, f64 | `%g`-style (no trailing zeros) |
| bool | `true` / `false` |
| null | `null` |
| binary | `<binary N bytes>` |
| fn | `<fn name>` or `<fn anonymous>` |
| vector | `<vector T length=N>` |
| map | `<map length=N>` |
| list | `<list length=N>` |
| userdata | `<userdata TypeName>` or `<userdata>` (plain) |

Returns nothing (void).

---

## file I/O

Registered by `BBL::addFileIo(bbl)`.  Provides `fopen`, `filebytes`, and the `File` typed userdata.

### fopen

```bbl
[= f [fopen "path.txt"]]          // read mode (default)
[= f [fopen "path.txt" "w"]]      // write mode
[= f [fopen "path.txt" "a"]]      // append mode
```

Opens a file and returns a `File` (typed userdata, refcounted).  Mode strings follow C `fopen()` conventions: `"r"` (default if omitted), `"w"`, `"a"`, `"rb"`, `"wb"`, etc.

Runtime error if the file cannot be opened.

### File type

`File` is a typed userdata registered via `TypeBuilder`.  When the last reference drops, the destructor flushes and closes the handle (RAII — no explicit `close` needed).

| method | signature | description |
|--------|-----------|-------------|
| `read` | `[f.read]` | read entire remaining contents as a string |
| `read-bytes` | `[f.read-bytes n]` | read up to `n` bytes, return `binary` |
| `write` | `[f.write str]` | write a string to the file |
| `write-bytes` | `[f.write-bytes blob]` | write a binary blob to the file |
| `close` | `[f.close]` | flush and close immediately (optional — destructor does this) |
| `flush` | `[f.flush]` | flush buffered output without closing |

`read` returns a string.  `read-bytes` returns a `binary`.  Both return an empty value at EOF.

`write` accepts a string.  `write-bytes` accepts a `binary`.  Both error on a file opened in read mode.

```bbl
// read a whole file
[= f [fopen "data.txt"]]
[= contents [f.read]]

// write line by line
[= out [fopen "log.txt" "w"]]
[out.write "hello\n"]
[out.write "world\n"]
// out closed at scope exit
```

### filebytes

```bbl
[= tex [filebytes "texture.png"]]
```

Creates a lazy file-backed `BblBinary` from an external file.  See [binary-data.md](binary-data.md) for full semantics (stat for size, lazy load on `data()` access, non-seekable sources rejected).

---

## math

Registered by `BBL::addMath(bbl)`.  All math functions operate on `f64` and return `f64`.  Integer arguments are promoted to `f64`.

| function | signature | description |
|----------|-----------|-------------|
| `sin` | `[sin x]` | sine (radians) |
| `cos` | `[cos x]` | cosine (radians) |
| `tan` | `[tan x]` | tangent (radians) |
| `asin` | `[asin x]` | arc sine |
| `acos` | `[acos x]` | arc cosine |
| `atan` | `[atan x]` | arc tangent |
| `atan2` | `[atan2 y x]` | two-argument arc tangent |
| `sqrt` | `[sqrt x]` | square root |
| `abs` | `[abs x]` | absolute value |
| `floor` | `[floor x]` | round toward −∞ |
| `ceil` | `[ceil x]` | round toward +∞ |
| `min` | `[min a b]` | smaller of two values |
| `max` | `[max a b]` | larger of two values |
| `pow` | `[pow base exp]` | exponentiation |
| `log` | `[log x]` | natural logarithm |
| `log2` | `[log2 x]` | base-2 logarithm |
| `log10` | `[log10 x]` | base-10 logarithm |
| `exp` | `[exp x]` | e^x |

`sqrt` of a negative number is a runtime error (not NaN — BBL does not expose IEEE special values).

### constants

`pi` and `e` are defined as `f64` variables in the root scope by `addMath`:

```bbl
[= circumference [* 2.0 pi r]]
```

---

## socket

Registered by `BBL::addSocket(bbl)`.  Provides TCP client and server sockets.

### connect (client)

```bbl
[= sock [connect "127.0.0.1" 8080]]
[sock.send "GET / HTTP/1.0\r\n\r\n"]
[= response [sock.recv]]
[sock.close]
```

`connect` takes a host string and an integer port.  Returns a `Socket` (typed userdata, refcounted).  Runtime error on connection failure.

### listen (server)

```bbl
[= server [listen "0.0.0.0" 9000]]
[= client [server.accept]]           // blocks until a connection arrives
[= msg [client.recv]]
[client.send "ack"]
[client.close]
[server.close]
```

`listen` binds and listens on a host/port.  Returns a `ServerSocket` (typed userdata).  `accept` blocks and returns a `Socket` for the incoming connection.

### Socket type

| method | signature | description |
|--------|-----------|-------------|
| `send` | `[sock.send str]` | send a string |
| `send-bytes` | `[sock.send-bytes blob]` | send a binary blob |
| `recv` | `[sock.recv]` | receive data as string (blocks until data available) |
| `recv-bytes` | `[sock.recv-bytes n]` | receive up to `n` bytes as `binary` |
| `close` | `[sock.close]` | shutdown and close (optional — destructor does this) |

`recv` returns an empty string at EOF (peer closed).  `send` on a closed socket is a runtime error.

### ServerSocket type

| method | signature | description |
|--------|-----------|-------------|
| `accept` | `[server.accept]` | block until a client connects, return `Socket` |
| `close` | `[server.close]` | stop listening (optional — destructor does this) |

Both `Socket` and `ServerSocket` are RAII — the destructor closes the underlying fd when the refcount hits zero.

### scope

Socket is a reasonable v1 inclusion because:
- BBL files may be used in tooling pipelines where scripts need to fetch data over the network before serializing it.
- The implementation wraps `socket()`/`connect()`/`bind()`/`listen()`/`accept()` — no external dependencies.
- Typed userdata + destructor means no resource leaks.

If socket support is not desired, omit `BBL::addSocket(bbl)` — the rest of the stdlib works without it.

---

## string methods

Registered by `BBL::addString(bbl)`.  **Deferred for v1** — see [backlog](../backlog.md) under "string operations".

Planned methods: `substring`, `index-of`, `replace`, `split`, `join`, `upper`, `lower`, `trim`, `to-int`, `to-f64`, `format`.

---

## open questions

None.
