# string operations

## goal

Basic string manipulation for scripting.  Strings are interned, immutable reference types.  Operations that produce new strings return new interned values.

## concatenation

`+` is variadic for strings — multiple string arguments are concatenated left-to-right:

```bbl
[= greeting [+ "hello" " " "world"]]   // "hello world"
```

The result is a new interned string.  Mixing string and non-string types is a type error (no implicit conversion).

## methods

| method | signature | description |
|--------|-----------|-------------|
| `length` | `[s.length]` | number of bytes (not characters — UTF-8 means these can differ) |

## comparison

- `==` / `!=` — pointer equality via interning.  O(1).
- `<` `>` `<=` `>=` — **type error**.  Use a library function for lexicographic ordering if needed.

## what strings cannot do (v1)

These are deferred to future work:

- Substring / slice
- Character indexing (UTF-8 complicates this)
- Search / find
- Replace
- Split / join
- Case conversion
- int-to-string / string-to-int conversion
- Format / printf-style interpolation

For v1, `print` handles output formatting (variadic, accepts any type).  String-building beyond `+` concatenation is deferred.

## interning and immutability

All strings are interned in a global table on `BblState`.  Creating a string hashes it and returns the existing instance if found.  Strings are **immutable** — there is no way to modify a string's contents.  Operations like `+` produce new strings.

Interned strings are **refcounted**.  When a string's refcount drops to zero (no variable, struct field, capture, or container holds it), the string is removed from the intern table and freed.  This keeps the table bounded by live references — scripts that generate many temporary strings via `+` do not leak memory.

## open questions

None — all resolved.  `typeof` is tracked in [backlog.md](../backlog.md).

1. **`typeof` result** — `[typeof "hello"]` should return `"string"`.  But `typeof` itself is not yet specified as a built-in.
