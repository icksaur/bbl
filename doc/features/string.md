# String Operations
Status: done

## Goal

Strings are the primary output medium for BBL scripts — file generation, text formatting, data serialization.  The current API handles construction (`+` with auto-coerce, `str`) but not inspection, formatting, or parsing.

Two problems must be solved together:

1. **Missing operations** — no substring, search, split, parse, format, pad.  The `1_file_gen` benchmark hand-rolled a 4-line `pad-left`; every script doing string formatting hits this wall.

2. **Intern table pollution** — every `intern()` call creates an entry that lives until `~BblState`.  Adding string operations that produce temporaries (`slice`, `replace`, `trim`, `+` in a loop) will bloat the intern table with strings no one references.  This must be fixed before adding operations that amplify the problem.

## Architecture: GC'd strings with weak interning

### The problem

Currently, `BblString` has no `marked` flag.  `gcMark()` ignores strings (`default: break`).  The intern table owns all strings and frees them only in `~BblState`.  Every `intern()` call that produces a unique string allocates permanently.

A loop like `(loop (< i 1000) (= s (+ s "x")) (= i (+ i 1)))` creates 1000 intern table entries — 999 are unreachable after the next iteration.

### The fix: add strings to the GC

```
intern("hello") → hash lookup
  → found:  return existing BblString*
  → not found: allocate BblString, add to allocatedStrings, add to internTable (weak)
```

Changes:

1. **Add `marked` flag to `BblString`** (like every other GC type).

2. **Mark strings in `gcMark()`** — add `case BBL::Type::String`.

3. **Sweep strings in `gc()`** — partition `allocatedStrings` on `marked`, delete unmarked, AND remove them from `internTable`.

4. **`internTable` becomes a weak lookup** — it still deduplicates (`intern` checks it first), but it doesn't keep strings alive.  The GC is the sole owner.

5. **Increment `allocCount` in `intern()`** when creating a new string (currently missing — strings don't trigger GC).

6. **Pointer equality stays valid** — `intern()` still guarantees that all live strings with the same content share one `BblString*`.  Two live values can only have the same content if they point to the same object.  No change to `operator==` or `bblValueKeyEqual`.

### Sweep detail

During sweep, for each unmarked string `s`:
- `internTable.erase(s->data)` — remove the weak reference
- `delete s` — free the memory

If a string with the same content is needed later, `intern()` allocates a fresh one.

### What this costs

- ~10 lines in `gc()` (identical pattern to existing binary/fn/struct/vector/table sweeps)
- ~3 lines in `gcMark()` (one new case)
- 1 line: `allocCount++` in `intern()` for new strings
- 1 line: `bool marked = false` in `BblString`
- Small per-GC cost: iterating `allocatedStrings` during sweep + erasing from the hash map

### What this enables

Every string operation below can freely call `intern()` for results.  Temporaries that go out of scope are collected.  The intern table stays compact — only reachable strings survive.

## Parsing builtins

### `int`

Parses a string to a 64-bit signed integer.  Also truncates floats (toward zero, like C cast).

```bbl
(int "42")            // 42
(int "-7")            // -7
(int " 42")           // 42 — leading whitespace accepted
(int "42abc")         // error — partial parse, trailing chars
(int "abc")           // error — no digits
(int 3.9)             // 3 — float truncation
(int -2.7)            // -2
```

Implementation: `strtoll` for strings, check full consumption (endptr must be at end of string, ignoring trailing whitespace is not allowed — only leading whitespace via strtoll's built-in behavior).  Float→int is `static_cast<int64_t>`.

### `float`

Parses a string to a 64-bit double.  Also widens ints.

```bbl
(float "3.14")        // 3.14
(float "-0.5")        // -0.5
(float "3.14x")       // error — partial parse
(float "bad")         // error
(float 42)            // 42.0
```

Implementation: `strtod` for strings, same full-consumption check.  Int→float is `static_cast<double>`.

Both `int` and `float` are top-level builtins (not methods) because they return a different type than the input.  Registered in `addPrint` alongside `str`.

## String methods

All methods dispatch via the existing `DotAccess` infrastructure.  Strings are immutable — methods return new values.  All indexing is byte-based (consistent with `length`).

### Access

| method | signature | description |
|--------|-----------|-------------|
| `length` | `s.length` | byte count (existing) |
| `at` | `(s.at i)` | single-byte string at index `i`. Throws on out-of-bounds |
| `slice` | `(s.slice start [end])` | substring from `start` (inclusive) to `end` (exclusive, default=length). Clamps silently |

```bbl
(= s "hello world")
(s.at 0)              // "h"
(s.at 11)             // error: out of bounds
(s.slice 0 5)         // "hello"
(s.slice 6)           // "world"
```

`at` is strict (throws), `slice` is lenient (clamps) — matching vector `at` vs Python slice conventions.

### Search

| method | signature | description |
|--------|-----------|-------------|
| `find` | `(s.find needle [start])` | byte index of first occurrence, or -1 |
| `contains` | `(s.contains sub)` | boolean |
| `starts-with` | `(s.starts-with prefix)` | boolean |
| `ends-with` | `(s.ends-with suffix)` | boolean |

```bbl
(= s "hello world")
(s.find "world")          // 6
(s.find "xyz")            // -1
(s.find "l" 3)            // 3 — search from offset
(s.contains "world")      // true
(s.starts-with "hello")   // true
(s.ends-with "world")     // true
```

### Transform

| method | signature | description |
|--------|-----------|-------------|
| `replace` | `(s.replace old new)` | replace all occurrences. Empty `old` is an error |
| `split` | `(s.split sep)` | table of substrings (0-based keys). Empty `sep` is an error |
| `join` | `(sep.join container)` | join table/vector elements with separator |
| `upper` | `s.upper` or `(s.upper)` | ASCII uppercase |
| `lower` | `s.lower` or `(s.lower)` | ASCII lowercase |
| `trim` | `s.trim` or `(s.trim)` | strip ASCII whitespace from both ends |
| `trim-left` | `s.trim-left` or `(s.trim-left)` | strip leading whitespace |
| `trim-right` | `s.trim-right` or `(s.trim-right)` | strip trailing whitespace |

```bbl
("aXbXc".replace "X" "-")  // "a-b-c"
("a,b,c".split ",")        // table {0:"a", 1:"b", 2:"c"}
(",".join items)            // "x,y,z"
("hello".upper)             // "HELLO"
("  hi  ".trim)             // "hi"
```

`join` iterates integer keys 0..length-1 for tables (matching `push`/`at`), all elements for vectors.  Each element is converted via `valueToString`.

`split`/`replace` with empty search string throw immediately — prevents the infinite loop from `std::string::find("")` returning 0 forever.

### Padding

| method | signature | description |
|--------|-----------|-------------|
| `pad-left` | `(s.pad-left width [fill])` | left-pad to `width`. Default fill = `" "` |
| `pad-right` | `(s.pad-right width [fill])` | right-pad to `width`. Default fill = `" "` |

```bbl
((str 42).pad-left 6)       // "    42"
((str 42).pad-left 6 "0")   // "000042"
((str 42).pad-right 6)      // "42    "
```

Fill must be a single-character string (throws otherwise).  If string is already >= width, returns unchanged.

This was the #1 formatting pain point — `pad-left` was hand-rolled as a 4-line BBL function in every benchmark script that did formatted output.

## Formatting: `fmt`

```bbl
(fmt "{} + {} = {}" 1 2 3)       // "1 + 2 = 3"
(fmt "name: {}" "Alice")          // "name: Alice"
(fmt "use {{}} for placeholders") // "use {} for placeholders"
```

`{}` is replaced by the next argument (converted via `valueToString`).  `{{` and `}}` produce literal braces.  Argument count must match placeholder count (throws on mismatch).

No format specifiers inside `{}` — use `pad-left`/`pad-right` and `str` for width/alignment control.  Composable primitives over embedded mini-languages:

```bbl
(fmt "{}|{}" ((str i).pad-left 4) ((str (* i j)).pad-left 4))
```

`fmt` builds the entire result in one `std::string` then interns once — avoids the O(n²) intern-per-concat cost of chained `+`.

Top-level builtin, registered in `addPrint`.

## Method dispatch

### No-arg methods (property-style)

`length`, `trim`, `trim-left`, `trim-right`, `upper`, `lower` work in both positions:

- Field access: `s.upper` (DotAccess eval, ~line 776)
- Call: `(s.upper)` (evalList string dispatch, ~line 1490)

### Methods with arguments

`at`, `slice`, `find`, `contains`, `starts-with`, `ends-with`, `split`, `join`, `replace`, `pad-left`, `pad-right` work only in call position.

### Why not a `string` table?

Lua puts string operations in a global `string` table: `string.find(s, "x")`.  Advantages: clean namespace, user-extensible.  Disadvantages:

- **Inconsistent** — vector and table already use `v.push`, `t.get`.  Switching to `string.find(s, "x")` but keeping `(t.get "key")` is confusing.
- **Requires callable-value fallback** — BBL tables throw on unknown method names.  `(string.find s "x")` would need tables to fall through to stored function values, which is a separate feature (prototype-based dispatch).
- **More verbose** — `(string.find s "x")` vs `(s.find "x")`.  For a language that's already verbose (prefix syntax), every saved token matters.

Methods keep the global namespace clean (they only exist in string context) and are consistent with the existing type method pattern.  If we later add callable-value fallback to tables, user-defined string extensions become possible, but the builtins should be methods.

## Concatenation

`+` is variadic when the left operand is a string.  Non-string operands are auto-coerced via `valueToString`:

```bbl
(+ "score: " 42)               // "score: 42"
(+ "pi=" 3.14 " ok=" true)     // "pi=3.14 ok=true"
```

This is already implemented.  *(Note: the current `string.md` says mixing types is a type error — that info is outdated.  The auto-coerce behavior was added alongside `str`.)*

## Comparison

- `==` / `!=` — pointer equality via interning.  O(1).  Remains valid after GC change (see architecture section).
- `<` `>` `<=` `>=` — **type error**.  Lexicographic ordering is deferred.

## Interning

All strings pass through `intern()`.  If the content already exists among live strings, the existing `BblString*` is returned (deduplication).  This guarantees pointer equality for identical content.

With GC'd strings, the intern table is a **weak lookup** — it enables deduplication but doesn't keep strings alive.  Unreachable strings are collected and removed from the intern table during GC sweep.

## Considerations

### Bytes vs characters

All operations are byte-indexed.  `length` returns bytes, `at` indexes bytes, `slice` slices bytes, `find` returns byte offsets.  Correct for ASCII; compatible with UTF-8 as long as users don't split multi-byte sequences.  For a serialization DSL, ASCII-level string processing is sufficient.

### Slice optimization: substring views

One could avoid interning substrings by storing a reference to the parent string + offset + length (like `std::string_view`).  This would save memory when slicing large strings.

**Deferred.**  The complexity cost is high:
- `BblString` becomes a discriminated union (owning data vs view into parent)
- The parent must be kept alive (GC reference from view to parent)
- `intern()` can't deduplicate views without hashing the view content
- Every method that reads string bytes must handle both representations

With GC'd strings, temporaries from `slice` are collected when unreachable.  The main cost is the `intern()` lookup for each slice, which is O(n) in substring length for hashing.  This is acceptable for BBL's target use case.  If profiling shows it matters, `allocString()` (skip intern) could be added as an optimization — at the cost of breaking pointer equality for those strings.

### No regex

`find`, `contains`, `split`, `replace` with literal strings cover the common cases.  `std::regex` is slow; external regex libraries add dependency weight.  Not needed for a serialization DSL.

### No mutability

Strings remain immutable.  All methods return new strings.  Consistent with interning.

### Performance of `+` in loops

`(+ s "x")` in a loop interns a progressively longer intermediate each iteration — O(n²) total for n iterations.  With GC'd strings, old intermediates are collected, so memory is bounded.  But the hashing cost remains O(n) per iteration.

`fmt` avoids this by building the result in one `std::string` then interning once.  Document: prefer `fmt` over chained `+` for multi-argument formatting.

For bulk string building (generating large files), a future `StringBuilder` / `buf` type could provide O(n) total cost.  Deferred.

### Error behavior summary

| operation | error condition | behavior |
|-----------|----------------|----------|
| `at` | out of bounds | throw |
| `slice` | out of range | clamp silently |
| `find` | not found | return -1 |
| `int` / `float` | parse failure | throw |
| `fmt` | arg count mismatch | throw |
| `split` / `replace` | empty search string | throw |
| `pad-left` / `pad-right` | fill not single char | throw |

## Acceptance

### GC'd strings
1. Strings created in a loop and immediately discarded are collected by GC (intern table doesn't grow unbounded).
2. Pointer equality still works after GC cycles — `(== "hello" "hello")` is true.
3. All existing 353 tests pass after GC change.

### Parsing builtins
4. `(int "42")` → 42
5. `(int "-7")` → -7
6. `(int "abc")` → throws
7. `(int "42abc")` → throws (partial parse rejected)
8. `(int " 42")` → 42 (leading whitespace accepted)
9. `(int 3.9)` → 3 (truncate)
10. `(float "3.14")` → 3.14
11. `(float "3.14x")` → throws (partial parse rejected)
12. `(float "bad")` → throws
13. `(float 42)` → 42.0

### String methods
14. `("hello".at 0)` → "h"
15. `("hello".at 5)` → throws
16. `("hello world".slice 0 5)` → "hello"
17. `("hello world".slice 6)` → "world"
18. `("hello world".find "world")` → 6
19. `("hello".find "xyz")` → -1
20. `("hello".find "l" 3)` → 3
21. `("hello world".contains "world")` → true
22. `("hello".starts-with "hel")` → true
23. `("hello".ends-with "llo")` → true
24. `("a,b,c".split ",")` → table {0:"a", 1:"b", 2:"c"}
25. `("abc".split "")` → throws (empty separator)
26. `(",".join t)` where t = {0:"x", 1:"y"} → "x,y"
27. `("aXbXc".replace "X" "-")` → "a-b-c"
28. `("abc".replace "" "x")` → throws (empty search)
29. `("  hi  ".trim)` → "hi"
30. `("  hi  ".trim-left)` → "hi  "
31. `("  hi  ".trim-right)` → "  hi"
32. `("hello".upper)` → "HELLO"
33. `("HELLO".lower)` → "hello"
34. `((str 42).pad-left 6)` → "    42"
35. `((str 42).pad-left 6 "0")` → "000042"
36. `((str 42).pad-right 6)` → "42    "

### Format
37. `(fmt "{} + {} = {}" 1 2 3)` → "1 + 2 = 3"
38. `(fmt "use {{}} for placeholders")` → "use {} for placeholders"
39. `(fmt "{}" 42)` → "42"
40. `(fmt "no args")` → "no args"
41. `(fmt "{} {}" 1)` → throws (arg count mismatch)

### Docs
42. `doc/bbl.md` updated with new builtins and methods.
43. `doc/spec.md` updated.
44. `doc/features/string.md` status set to done.
