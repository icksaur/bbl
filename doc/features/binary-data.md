# binary data

## goal

First-class binary blobs that can embed arbitrary byte sequences (textures, audio, mesh data) directly in `.bbl` files or streams.  When loading from a **seekable file**, binary data is **lazy-loaded** — the interpreter skips past the raw bytes during parsing and only reads them into memory when the value is actually accessed.  When loading from a **non-seekable source** (stdin, pipes), the bytes are read immediately into memory since the stream cannot be rewound.  This makes it practical to embed megabytes of asset data in a script file without paying the memory cost upfront, while still supporting pipe-based workflows.

## type

`binary` is a **reference type** (refcounted, shared on assignment).  It represents an opaque byte buffer.

A `binary` value has one of two internal representations, depending on its source:

- **File-backed (lazy)**: holds a file path, byte offset, and byte count.  The actual bytes are not in memory.  On first access, the runtime seeks to the offset, reads the bytes, and transitions to loaded state.  Only possible when the source is a seekable file.
- **In-memory (immediate)**: holds an allocated byte buffer.  Created when parsing from a non-seekable source (stdin, pipes), when constructed from C++ via `setBinary()`, or after a lazy blob is loaded.

Both representations expose the **same API** — scripts and C functions cannot distinguish them.  The only observable difference is that `loaded` returns `false` for an unaccessed file-backed blob and `true` for an in-memory blob.

## literal syntax

```
0b<size>:<raw bytes>
```

- `0b` prefix distinguishes from hex (`0x`) and decimal.
- `<size>` is a decimal integer: the exact byte count.
- `:` begins the raw region.  The lexer reads exactly `<size>` bytes verbatim — no escaping, no character interpretation, no encoding.
- There is no terminator.  The lexer knows the length from `<size>`.

Bencode uses the same `<size>:<data>` convention.

```bbl
[= texture 0b65536:<65536 raw bytes>]
[= audio 0b1048576:<1048576 bytes of PCM data>]
```

## lexer behavior

When the lexer encounters `0b`:

1. Parse the decimal size `N`.
2. Read `:`.
3. **If the source is seekable** (regular file):
   a. Record the current file offset as `blob_offset`.
   b. Seek forward `N` bytes (do not read them into memory).
   c. Emit a `BinaryLiteral` token carrying `{source_file, blob_offset, N}`.
4. **If the source is non-seekable** (stdin, pipe):
   a. Allocate `N` bytes.
   b. Read exactly `N` bytes into the buffer.
   c. Emit a `BinaryImmediate` token carrying `{data_ptr, N}`.

For seekable sources, the lexer **does not allocate a buffer** for the blob.  It records the metadata and moves on.  This is the core of lazy loading.

For non-seekable sources, the data must be consumed immediately since you cannot seek back.  The blob starts in loaded state.

## interpreter behavior

Evaluating a `BinaryLiteral` (file-backed) produces a `BblValue` of type `binary` in its **unloaded** state:

```
BblBinary {
    state: unloaded
    file: <source file handle or path>
    offset: <byte offset into file>
    size: <byte count>
    data: nullptr
}
```

Evaluating a `BinaryImmediate` (stdin/pipe) produces a `BblValue` of type `binary` in its **loaded** state:

```
BblBinary {
    state: loaded
    file: nullptr
    offset: 0
    size: <byte count>
    data: <allocated buffer>
}
```

The `BblState` keeps the source file handle open (or retains the path for reopening) so that lazy reads can seek back into it.  Non-seekable sources do not need file handles after parsing.

### loading

On first access — passing to a C function, iterating bytes, writing to another file, copying to a `vector`, etc. — the runtime:

1. Opens or seeks the file handle to `offset`.
2. Allocates `size` bytes.
3. Reads `size` bytes into the buffer.
4. Transitions state to `loaded`, sets `data` pointer.
5. Subsequent accesses use the in-memory buffer directly.

Once loaded, the blob stays in memory until its refcount drops to zero.

### file handle management

**File-backed blobs:** The `BblState` owns the file handles for all source files being executed.  Multiple `binary` values from the same `.bbl` file share a single file handle (or at least a single path — the runtime may reopen as needed).

When `~BblState` runs, all file handles are closed.

If a `binary` value outlives its source file's execution scope (e.g. returned from `exec` into the caller), the runtime must ensure the file handle or path remains valid for lazy loading.  Simplest approach: the `BblBinary` holds a refcounted handle to a `BblSourceFile` object that keeps the file path and manages open/close.

**In-memory blobs (stdin/pipe):** No file handle dependency.  The data is fully buffered at parse time.  No `BblSourceFile` is created for the non-seekable source.

## ownership

- `binary` is refcounted.  `[= b a]` increments the refcount.  Both names point to the same blob.
- Unloaded blobs are cheap — just a few words of metadata.
- Loaded blobs own their allocated buffer.  Buffer freed when refcount hits zero.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `length` | `[blob.length]` | byte count (available without loading) |
| `loaded` | `[blob.loaded]` | `bool` — whether bytes are in memory |

`length` does not trigger a load — the size is known from the literal.  Any other access (passing to a C function, iterating, etc.) triggers loading.

## mutability

Once loaded, a binary blob's buffer is **mutable in memory**.  Script code and C++ can write into the buffer.  The blob is a live byte array, not a frozen snapshot.

The underlying file is not modified — mutations happen in the in-memory copy only.  The source file remains as it was when the `.bbl` file was written.

## file modification during execution

Applies only to file-backed (lazy) blobs.  In-memory blobs are unaffected since they have no file reference.

If the source `.bbl` file is modified while the program is running, behavior is **exactly what you'd expect from raw file I/O**:

- Unloaded blobs that are accessed after the file changes will read whatever bytes are now at that offset.
- If the file has been truncated or the offset is now past EOF, the lazy load **fails with an error** (see [errors.md](errors.md)).
- Already-loaded blobs are unaffected — they have their own in-memory buffer.

No checksumming, no integrity validation.  The runtime reads bytes at an offset.  If the file moved under you, you get garbage or an error.

## multiple blobs per file

Several `0b` literals in one `.bbl` file all reference the same source file at different offsets.  The shared `BblSourceFile` handle covers this naturally — each `BblBinary` records its own `{offset, size}` tuple against the same file.

## `filebytes` — lazy binary from an external file

`filebytes` creates a file-backed `BblBinary` that references an external file's entire contents, without reading the data into memory.

```bbl
[= tex [filebytes "texture.png"]]
```

This produces the same lazy `BblBinary` as an inline `0b` literal — same type, same API — but instead of referencing an offset within the `.bbl` source file, it references the external file from offset 0 to EOF.

### comparison

```bbl
// both produce identical BblBinary values (same type, same API)
[= inline-bytes 0b10:helloworld]       // lazy ref to hello.bbl at offset, size=10
[= file-bytes [filebytes "hello.txt"]] // lazy ref to hello.txt at offset 0, size from stat
```

### behavior

1. `filebytes` calls `stat()` on the file to get its size.  If the file does not exist or is not readable, runtime error.
2. Creates a `BblBinary` in **unloaded** state:

```
BblBinary {
    state: unloaded
    file: "hello.txt"    // resolved path
    offset: 0            // always 0 — whole file
    size: <from stat>
    data: nullptr
}
```

3. `length` returns the stat'd size immediately — no data read.
4. `data()` or any content access triggers the lazy load (same as inline blobs).

### path resolution

Follows the same rules as `exec`: relative to the calling script's directory.  If `/foo/scene.bbl` does `[filebytes "texture.png"]`, it resolves to `/foo/texture.png`.  From C++ `bbl.exec()`, relative to CWD or the configured base path.

### non-seekable files

`filebytes` requires a seekable file (regular file on disk).  Passing a path to a pipe, device, or `/dev/stdin` is a runtime error — use stdin-based `0b` parsing for non-seekable sources.

### file modification

Same semantics as inline blobs: if the external file is modified after `filebytes` but before the blob is loaded, the load reads whatever bytes are there.  If the file is truncated or deleted, the lazy load fails with an error.  Already-loaded blobs are unaffected.

### C++ equivalent

```cpp
// C++ can achieve the same thing:
bbl.setFileBinary("tex", "/path/to/texture.png");  // lazy, no data read
```

## C++ API

```cpp
bbl.exec("scene.bbl");

// get the binary — may still be unloaded
auto* tex = bbl.getBinary("player-texture");
// tex->length() is always available
// tex->data() triggers lazy load if needed, returns uint8_t*
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data());

// register a binary blob from C++ memory
bbl.setBinary("generated-data", myPtr, mySize);  // copies into managed buffer
```

`bbl.setBinary()` copies the provided bytes into a `BblBinary` that starts in the loaded state (no file reference, no lazy loading — the data is already in memory).

## serialization workflow

Binary blobs are the reason BBL exists.  A serializer writes `.bbl` files with embedded asset data:

```bbl
[= vertex [struct [f32 x f32 y f32 z]]]
[= player-verts [vector vertex [0 1 0] [1 0 0] [-1 0 0]]]
[= player-texture 0b65536:<65536 bytes of png>]
[= player-audio 0b1048576:<1048576 bytes of wav>]
```

C++ loads the script.  The struct definitions and vectors parse instantly.  The texture and audio blobs are skipped during parse — only loaded when C++ calls `tex->data()`.  A scene with 100MB of textures parses in microseconds if no textures are accessed.

## generating .bbl files with binary data

From a shell:

```bash
SIZE=$(stat -c%s texture.png)
printf '[= texture 0b%d:' "$SIZE" > scene.bbl
cat texture.png >> scene.bbl
printf ']\n' >> scene.bbl
```

From C++:

```cpp
void writeBinary(FILE* out, const char* name, const uint8_t* data, size_t size) {
    fprintf(out, "[= %s 0b%zu:", name, size);
    fwrite(data, 1, size, out);
    fprintf(out, "]\n");
}
```

## open questions

None — all resolved.
