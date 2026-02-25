# Backlog

- **vector: resize / reserve** — expose capacity management (e.g. `(verts.reserve 1000)`).  For now, implementation grows like `std::vector` with amortized O(1) push.
- **vector: slice** — `(verts.slice 0 3)` returning a new vector.
- **script-level error handling** — `try`/`catch` or similar within BBL scripts.  For v1, all errors propagate to the C++ caller as `BBL::Error` exceptions.
- **C function multiple return values** — allow C functions to push more than one return value (like Lua).  For v1, one return value is sufficient.
- **break / continue** — early exit from `loop`.  `break` is higher priority than `continue`.
- **bitwise operators** — `band`, `bor`, `bxor`, `bnot`, `shl`, `shr` or similar.
- **typeof** — `(typeof val)` returning a type name string.  Enables generic functions that inspect argument types at runtime.
- **string operations** — substring, search, replace, split/join, case conversion, int↔string conversion, format/printf.  See [features/string.md](features/string.md).
- **string ordering** — lexicographic `<`/`>` for strings.  Currently `==`/`!=` only.
- **negative indexing** — `(verts.at -1)` for last element (Python-style).  Currently out-of-bounds error.
- **optional userdata destructor timing** — explore a way to guarantee destructor runs at scope exit for specific userdata types (RAII pattern), rather than relying on GC timing.
- **open filesystem access** — allow `filebytes` and `execfile` to access paths outside the script sandbox (absolute paths, parent traversal).  Requires explicit opt-in via `BblState` configuration.  Currently, script-level file access is restricted to the script's directory and children (see [features/security.md](features/security.md)).
