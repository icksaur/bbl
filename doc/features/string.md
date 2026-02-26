# String Operations

## goal

Basic string manipulation for scripting.  Strings are interned, immutable, GC-managed.  Operations that produce new strings return new interned values.

## concatenation

`+` is variadic for strings — multiple string arguments are concatenated left-to-right:

```bbl
(= greeting (+ "hello" " " "world"))   // "hello world"
```

The result is a new interned string.  Mixing string and non-string types is a type error (no implicit conversion).

## methods

| method | signature | description |
|--------|-----------|-------------|
| `length` | `(s.length)` | number of bytes (not characters — UTF-8 means these can differ) |

## comparison

- `==` / `!=` — pointer equality via interning.  O(1).
- `<` `>` `<=` `>=` — **type error**.  Use a library function for lexicographic ordering if needed (deferred).

## interning

All strings are interned in a global table on `BblState`.  Creating a string hashes it and returns the existing instance if found.  Strings are **immutable** — there is no way to modify a string's contents.  Operations like `+` produce new strings.

Interned strings live for the **lifetime of `BblState`**.  They are never evicted from the intern table — `~BblState` frees the entire table.  This avoids coupling the intern table to the GC.  For a serialization DSL that runs and exits, the memory cost is negligible.

## what strings cannot do (v1)

These are deferred to future work (see [backlog](../backlog.md)):

- Substring / slice
- Character indexing (UTF-8 complicates this)
- Search / find
- Replace
- Split / join
- Case conversion
- int-to-string / string-to-int conversion
- Format / printf-style interpolation

For v1, `print` handles output formatting (variadic, accepts any type).  String-building beyond `+` concatenation is deferred.

## open questions

None.
