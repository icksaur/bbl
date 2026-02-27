# String Operations
Status: proposed

## Goal

Add string methods for formatting, parsing, searching, and manipulation.  Strings are the primary output medium for BBL scripts (file generation, text formatting, data serialization).  The current string API — `length`, `str`, and `+` with auto-coerce — handles construction but not inspection, formatting, or parsing.  This feature fills that gap with a small, orthogonal set of methods and builtins.

## Design

### Parsing builtins: `int`, `float`

Two functions that parse a string into a number.  These are top-level builtins (not methods) because they return a different type than their input — calling `"42".int` would be odd given BBL's method dispatch returns the same receiver type elsewhere.

```bbl
(= n (int "42"))          // 42 (Int)
(= n (int "-7"))          // -7
(= f (float "3.14"))      // 3.14 (Float)
(= f (float "-0.5"))      // -0.5
(= n (int "abc"))         // error: cannot parse "abc" as int
(= f (float "nope"))      // error: cannot parse "nope" as float
```

`int` also accepts Float input (truncates toward zero, like C cast):

```bbl
(= n (int 3.9))           // 3
(= n (int -2.7))          // -2
```

`float` also accepts Int input (widens):

```bbl
(= f (float 42))          // 42.0
```

Implementation: ~20 lines each.  `int` on strings uses `strtoll`, checks for full consumption (trailing chars → error).  `float` on strings uses `strtod`, checks for full consumption.  Leading whitespace is accepted (consistent with `strtoll`/`strtod` behavior).  Partial parses like `"42abc"` are errors.  Type coercion cases are trivial.  Register in `addPrint` alongside `str`.

### String methods

All string methods are dispatched in the `DotAccess` handler for `BBL::Type::String`, same pattern as vector/table methods.  Since strings are immutable, all methods return new values (new strings or other types).

#### Character/byte access: `at`

```bbl
(= s "hello")
(s.at 0)                  // "h" — single-character string
(s.at 4)                  // "o"
(s.at 5)                  // error: index 5 out of bounds (length 5)
(s.at -1)                 // error: negative index
```

Returns a new 1-character string.  Operates on bytes, not Unicode code points (consistent with `length` being byte count).  This is fine for ASCII-dominated serialization use cases — document that it's byte-indexed.

Implementation: bounds check, then `intern(std::string(1, s.data[idx]))`.

#### Substring: `slice`

```bbl
(= s "hello world")
(s.slice 0 5)             // "hello"
(s.slice 6 11)            // "world"
(s.slice 6)               // "world" — omit end = rest of string
(s.slice 0 0)             // ""
```

Two arguments: `start` (inclusive), `end` (exclusive, optional — defaults to length).  Clamps out-of-range indices silently (matching Python/JS behavior — slice is lenient, `at` is strict).

Implementation: clamp start/end to [0, len], `intern(s.data.substr(start, end - start))`.

#### Search: `find`

```bbl
(= s "hello world")
(s.find "world")          // 6
(s.find "xyz")            // -1 (not found)
(s.find "l")              // 2 (first occurrence)
(s.find "l" 3)            // 3 (search from offset 3)
```

Returns the byte index of the first occurrence, or -1 if not found.  Optional second argument is the start position.

Implementation: `std::string::find`, return `npos` as -1.

#### Check: `contains`, `starts-with`, `ends-with`

```bbl
(= s "hello world")
(s.contains "world")      // true
(s.starts-with "hello")   // true
(s.ends-with "world")     // true
(s.starts-with "world")   // false
```

Boolean predicates.  Thin wrappers over find/compare.

Implementation: `starts-with` checks `s.data.compare(0, arg.size(), arg) == 0`.  `ends-with` checks the suffix.  `contains` uses `find != npos`.

#### Split and join: `split`, `join`

```bbl
(= s "a,b,c")
(= parts (s.split ","))   // table: {0: "a", 1: "b", 2: "c"}

(= sep "-")
(= t (table))
(t.push "x") (t.push "y") (t.push "z")
(sep.join t)               // "x-y-z"
```

`split` returns a table with 0-based integer keys (consistent with `push`).  Empty separator is an error (avoids infinite loop from `find("")` returning 0 forever).

`join` is called on the separator string with a table/vector argument.  For tables, `join` iterates integer keys 0 through length-1 in order (matching `push`/`at` semantics), ignoring string keys.  Each element is converted via `valueToString` (same as `+` auto-coerce).  For vectors, iterates all elements in order.

Implementation: `split` uses a loop over `std::string::find`; guard against empty separator at entry.  `join` iterates the container, calling `valueToString` per element, inserting the separator between them.

#### Replace: `replace`

```bbl
(= s "hello world world")
(s.replace "world" "BBL")         // "hello BBL BBL" — all occurrences
```

Replaces all occurrences.  Returns a new string.  Replace-first is rarely needed; replace-all is the common case (consistent with Python `str.replace`).  Empty search string is an error.

Implementation: guard against empty search string at entry, then loop over `find`, build result with `substr` + replacement.

#### Trim: `trim`, `trim-left`, `trim-right`

```bbl
(= s "  hello  ")
(s.trim)                  // "hello"
(s.trim-left)             // "hello  "
(s.trim-right)            // "  hello"
```

Removes ASCII whitespace (space, tab, newline, carriage return).  No arguments.

Implementation: `find_first_not_of` / `find_last_not_of` on `" \t\n\r"`.

#### Case: `upper`, `lower`

```bbl
("hello".upper)           // "HELLO"
("HELLO".lower)           // "hello"
```

ASCII-only case conversion.  Not locale-aware — appropriate for a serialization DSL.

Implementation: `std::transform` with `toupper`/`tolower`, intern result.

#### Padding: `pad-left`, `pad-right`

```bbl
(= s (str 42))
(s.pad-left 6)            // "    42" — pad with spaces to width 6
(s.pad-left 6 "0")        // "000042" — pad with "0"
(s.pad-right 6)           // "42    "
(s.pad-right 6 "-")       // "42----"
```

`pad-left width [fill]` — left-pad to `width` characters.  Default fill is space.  If string is already >= width, returns unchanged.  Fill must be a single-character string.

This was the #1 bblbench string pain point — `pad-left` was hand-rolled as a 4-line function in `1_file_gen.bbl`.

Implementation: if `s.size() >= width`, return as-is.  Otherwise prepend `(width - s.size())` copies of fill char.

### Formatting: `fmt`

A variadic formatting function using `{}` placeholders:

```bbl
(fmt "{} + {} = {}" 1 2 (+ 1 2))   // "1 + 2 = 3"
(fmt "name: {}" "Alice")            // "name: Alice"
(fmt "pi ~= {}" 3.14159)           // "pi ~= 3.14159"
```

`{}` is replaced by the next argument, converted via `valueToString`.  Literal `{` and `}` are escaped as `{{` and `}}`:

```bbl
(fmt "use {{}} for placeholders")   // "use {} for placeholders"
```

No format specifiers inside `{}` (no width, precision, alignment).  Use `pad-left`/`pad-right` and `str` for formatting control — composable primitives over embedded mini-languages.  This keeps `fmt` simple and avoids implementing a format spec parser.

```bbl
// Formatted table output — composable approach
(fmt "{}|{}" ((str i).pad-left 4) ((str (* i j)).pad-left 4))
```

Argument count must match placeholder count.  Mismatch is an error.

Implementation: ~30 lines.  Scan the format string for `{}`, `{{`, `}}`.  Build result by copying literal segments and inserting `valueToString(arg)` at each `{}`.  Register as a top-level builtin in `addPrint`.

### Method dispatch summary

New string methods add cases to two locations in `bbl.cpp`:

1. **Field access** (DotAccess eval, ~line 776): `length` already exists.  Add no-arg methods that work as properties: `trim`, `upper`, `lower`, `trim-left`, `trim-right`.

2. **Method call** (evalList string dispatch, ~line 1490): `length` already exists.  Add all methods: `at`, `slice`, `find`, `contains`, `starts-with`, `ends-with`, `split`, `join`, `replace`, `pad-left`, `pad-right`, `trim`, `trim-left`, `trim-right`, `upper`, `lower`.

No-arg methods (`trim`, `upper`, `lower`, `trim-left`, `trim-right`) work in both field-access position (`s.upper`) and call position (`(s.upper)`).  Methods with arguments (`at`, `slice`, `find`, etc.) only work in call position.

### Performance

All methods create new interned strings.  For typical serialization scripts (building output strings in a loop), this is fine — the intern table deduplicates identical results and the GC... doesn't collect strings (they live until `~BblState`).

The one concern is `+` in a loop building a large string — each iteration interns a progressively longer intermediate.  This is O(n²) for n iterations.  Acceptable for BBL's target use case (scripts that run and exit), but worth documenting.  A future `StringBuilder` or `buf` type could address this if profiling shows it matters.

`fmt` avoids the intern-per-concat problem — it builds the entire result in one `std::string`, then interns once.  Prefer `fmt` over chained `+` for multi-argument formatting.

## Considerations

### Why methods, not functions?

`(s.find "x")` reads better than `(find s "x")` and is consistent with vector/table API.  BBL already has the method dispatch infrastructure.  Methods keep the global namespace clean.

### Why `int`/`float` as builtins, not methods?

Type conversion functions are conceptually not operations *on* a string — they produce a different type.  `(int "42")` reads as "make an int from this", which is clearer than `("42".int)`.  Also, `int`/`float` accept non-string inputs (type widening/narrowing), which wouldn't make sense as string methods.

### Bytes vs characters

All operations are byte-indexed.  `length` returns bytes, `at` indexes bytes, `slice` slices bytes, `find` returns byte offsets.  This is correct for ASCII and compatible with UTF-8 as long as users don't split multi-byte sequences.  For a serialization DSL, ASCII-level string processing is sufficient.  Document this clearly.

### No regex

Regex is a large dependency (either `std::regex` which is slow, or an external library).  BBL's target use case doesn't need pattern matching — `find`, `contains`, `split`, and `replace` with literal strings cover the common cases.

### No mutability

Strings remain immutable.  All methods return new strings.  This is consistent with interning and simpler to reason about.  No need for `StringBuilder` at this stage.

### string.md is outdated

`doc/features/string.md` currently states: "Mixing string and non-string types is a type error (no implicit conversion)."  This is wrong — `+` auto-coerces non-string operands via `valueToString` as of the `str`/auto-coerce feature.  Acceptance criterion #36 must correct this along with adding the new methods.

### Error handling

- `at`: throws on out-of-bounds (strict, like vector.at)
- `slice`: clamps silently (lenient, like Python)
- `find`: returns -1 (sentinel, like C/C++)
- `int`/`float`: throws on parse failure (explicit error, no silent NaN)
- `fmt`: throws on arg count mismatch
- `pad-left`/`pad-right`: fill string must be exactly 1 character (throws otherwise)

### Rejected alternatives

- **`format` with specifiers** (`{:>6}`, `{:.2f}`): adds a mini-language parser inside BBL.  Composable primitives (`pad-left`, `str`) achieve the same result with less implementation complexity and more flexibility.
- **`charAt` returning int**: returning a 1-char string is more useful (can be concatenated directly).  BBL has no char type.
- **`split` with limit**: deferred.  Can be added later without breaking changes.
- **Mutable `StringBuilder`**: deferred.  Current `+` and `fmt` are adequate for scripts that run and exit.
- **`ord`/`chr`**: deferred.  Byte↔int conversion is rarely needed in serialization scripts.

## Acceptance

### Parsing builtins
1. `(int "42")` → 42
2. `(int "-7")` → -7
3. `(int "abc")` → throws
4. `(int "42abc")` → throws (partial parse rejected)
5. `(int " 42")` → 42 (leading whitespace accepted)
6. `(int 3.9)` → 3 (truncate)
7. `(float "3.14")` → 3.14
8. `(float "3.14x")` → throws (partial parse rejected)
9. `(float "bad")` → throws
10. `(float 42)` → 42.0

### String methods
11. `("hello".at 0)` → "h"
12. `("hello".at 5)` → throws (out of bounds)
13. `("hello world".slice 0 5)` → "hello"
14. `("hello world".slice 6)` → "world"
15. `("hello world".find "world")` → 6
16. `("hello".find "xyz")` → -1
17. `("hello".find "l" 3)` → 3
18. `("hello world".contains "world")` → true
19. `("hello".starts-with "hel")` → true
20. `("hello".ends-with "llo")` → true
21. `("a,b,c".split ",")` → table {0:"a", 1:"b", 2:"c"}
22. `("abc".split "")` → throws (empty separator)
23. `(",".join t)` where t = {0:"x", 1:"y"} → "x,y"
24. `("aXbXc".replace "X" "-")` → "a-b-c"
25. `("abc".replace "" "x")` → throws (empty search string)
26. `("  hi  ".trim)` → "hi"
27. `("  hi  ".trim-left)` → "hi  "
28. `("  hi  ".trim-right)` → "  hi"
29. `("hello".upper)` → "HELLO"
30. `("HELLO".lower)` → "hello"
31. `((str 42).pad-left 6)` → "    42"
32. `((str 42).pad-left 6 "0")` → "000042"
33. `((str 42).pad-right 6)` → "42    "

### Format
34. `(fmt "{} + {} = {}" 1 2 3)` → "1 + 2 = 3"
35. `(fmt "use {{}} for placeholders")` → "use {} for placeholders"
36. `(fmt "{}" 42)` → "42"
37. `(fmt "no args")` → "no args"
38. `(fmt "{} {}" 1)` → throws (arg count mismatch)

### Integration
39. All existing 353 tests still pass.
40. `doc/bbl.md` updated with new string methods, `int`, `float`, `fmt`.
41. `doc/features/string.md` updated (fix outdated `+` auto-coerce docs, add new methods).
42. `doc/spec.md` updated.
