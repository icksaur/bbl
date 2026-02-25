# backlog

- **vector: resize / reserve** — expose capacity management (e.g. `[verts.reserve 1000]`).  For now, implementation grows like `std::vector` with amortized O(1) push.
- **vector: slice** — `[verts.slice 0 3]` returning a new vector.
- **script-level error handling** — `try`/`catch` or similar within BBL scripts.  For v1, all errors propagate to the C++ caller as `BBL::Error` exceptions.  In-script handling is deferred.
- **C function multiple return values** — allow C functions to push more than one return value (like Lua).  For v1, one return value is sufficient.  Multi-return adds stack management complexity.
- **box (heap-wrapped structs)** — `[box foo]` wraps a value-type struct in a refcounted heap allocation, enabling pass-by-reference mutation (`[boxed.inc]` mutates in place).  Unboxed structs remain C++ compatible.
- **break / continue** — early exit from `loop` and `for`.  `break` is higher priority than `continue`.
- **bitwise operators** — `band`, `bor`, `bxor`, `bnot`, `shl`, `shr` or similar.
- **typeof** — `[typeof val]` returning a type name string.  Enables generic functions that inspect argument types at runtime.
- **string operations** — substring, search, replace, split/join, case conversion, int↔string conversion, format/printf.  See [string.md](features/string.md).
- **string ordering** — lexicographic `<`/`>` for strings.  Currently `==`/`!=` only.
- **negative indexing** — `[verts.at -1]` for last element (Python-style).  Currently out-of-bounds error.
- **weak references (design decided, implementation deferred)** — `[weak x]` returns a non-owning handle that does not increment `x`'s refcount.  `[deref w]` returns the referent or `null` if it has been freed.  Solves indirect refcount cycles (A→B→weak A) by letting the programmer mark one link as non-owning.  Works for any container type (map, list, closures).  Replaces the previous plan for transitive cycle detection (which was O(n) per insertion).