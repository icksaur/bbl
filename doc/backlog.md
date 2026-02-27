# Backlog

- **optional userdata destructor timing** — explore a way to guarantee destructor runs at scope exit for specific userdata types (RAII pattern), rather than relying on GC timing.

---

- **vector: resize / reserve** — expose capacity management (e.g. `(verts.reserve 1000)`).  For now, implementation grows like `std::vector` with amortized O(1) push.
- **vector: slice** — `(verts.slice 0 3)` returning a new vector.
- **script-level error handling** — `try`/`catch` or similar within BBL scripts.  For v1, all errors propagate to the C++ caller as `BBL::Error` exceptions.
- **C function multiple return values** — allow C functions to push more than one return value (like Lua).  For v1, one return value is sufficient.
- **break / continue** — early exit from `loop`.  `break` is higher priority than `continue`.
- **string ordering** — lexicographic `<`/`>` for strings.  Currently `==`/`!=` only.
- **negative indexing** — `(verts.at -1)` for last element (Python-style).  Currently out-of-bounds error.


