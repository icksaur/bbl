# binary data

## goal

First-class binary blobs that can embed arbitrary byte sequences (textures, audio, mesh data) directly in `.bbl` files or streams.  Binary data is read immediately into memory during parsing.

## type

`binary` is a **GC-managed type** (shared on assignment).  It represents a byte buffer: `{uint8_t* data, size_t length}`.

## literal syntax

```
0b<size>:<raw bytes>
```

- `0b` prefix distinguishes from decimal.
- `<size>` is a decimal integer: the exact byte count.
- `:` begins the raw region.  The lexer reads exactly `<size>` bytes verbatim — no escaping, no character interpretation, no encoding.
- There is no terminator.  The lexer knows the length from `<size>`.

Bencode uses the same `<size>:<data>` convention.

```bbl
(= texture 0b65536:<65536 raw bytes>)
(= audio 0b1048576:<1048576 bytes of PCM data>)
```

## lexer behavior

When the lexer encounters `0b`:

1. Parse the decimal size `N`.
2. Read `:`.
3. Allocate `N` bytes.
4. Read exactly `N` bytes into the buffer.
5. Emit a `BinaryLiteral` token carrying `{data_ptr, N}`.

The data is always read immediately — whether the source is a file, stdin, or a pipe.

## interpreter behavior

Evaluating a `BinaryLiteral` produces a `BblValue` of type `binary`:

```
BblBinary {
    data: <allocated buffer>
    length: <byte count>
}
```

## ownership

- `binary` is GC-managed.  `(= b a)` where `a` is a binary makes both names point to the same buffer.
- The GC frees the buffer when no references remain.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `length` | `(blob.length)` | byte count |

## mutability

A binary blob's buffer is **mutable in memory**.  Script code and C++ can write into the buffer.

## `filebytes` — read an external file into a binary

`filebytes` reads an external file's entire contents into a `BblBinary`.

```bbl
(= tex (filebytes "texture.png"))
```

### behavior

1. Open the file.  If it does not exist or is not readable, runtime error.
2. Read the entire file into an allocated buffer.
3. Return a `BblBinary` with the file's contents.

### path resolution

Follows the same rules as `execfile`: relative to the calling script's directory.  From C++ `bbl.execfile()`, relative to CWD or the configured base path.

**Sandboxing**: from script, `filebytes` can only access files in the calling script's directory or child directories.  Absolute paths and paths containing `..` are a runtime error.  See [security.md](security.md).

### C++ equivalent

```cpp
bbl.setBinary("tex", data_ptr, data_size);  // copies into managed buffer
```

## C++ API

```cpp
bbl.execfile("scene.bbl");

auto* tex = bbl.getBinary("player-texture");
size_t len = tex->length();
const uint8_t* data = tex->data();
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

// create a binary from C++ memory
bbl.setBinary("generated-data", myPtr, mySize);  // copies into managed buffer
```

## serialization workflow

Binary blobs are the reason BBL exists.  A serializer writes `.bbl` files with embedded asset data:

```bbl
// vertex and mesh types registered from C++

(= player-verts (vector vertex
    (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)
))
(= player-texture 0b65536:<65536 bytes of png>)
(= player-audio 0b1048576:<1048576 bytes of wav>)
```

C++ loads the script.  The texture and audio blobs are read into memory at parse time.

## generating .bbl files with binary data

From a shell:

```bash
SIZE=$(stat -c%s texture.png)
printf '(= texture 0b%d:' "$SIZE" > scene.bbl
cat texture.png >> scene.bbl
printf ')\n' >> scene.bbl
```

## open questions

None.
