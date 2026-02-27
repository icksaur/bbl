# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [code-quality.md](code-quality.md).

---

## String Operations

Full spec: `doc/features/string.md`

### Phase 1: GC'd strings

[ ] 1.1 In `bbl.h`, add `bool marked = false;` to `struct BblString` (after `std::string data;`).

[ ] 1.2 In `bbl.cpp` `intern()`, add `allocCount++;` after `internTable[str->data] = str;` (end of the new-string path, consistent with other allocators).

[ ] 1.3 In `bbl.cpp` `gcMark()`, add a `case BBL::Type::String:` before `default: break;`:
```cpp
case BBL::Type::String:
    if (val.stringVal && !val.stringVal->marked) {
        val.stringVal->marked = true;
    }
    break;
```

[ ] 1.4 In `bbl.cpp` `gc()` sweep section, after the userdata sweep block, add string sweep:
```cpp
// Sweep strings
{
    auto mid = std::partition(allocatedStrings.begin(), allocatedStrings.end(),
                              [](BblString* s) { return s->marked; });
    for (auto it = mid; it != allocatedStrings.end(); ++it) {
        internTable.erase((*it)->data);
        delete *it;
    }
    allocatedStrings.erase(mid, allocatedStrings.end());
    for (auto* s : allocatedStrings) s->marked = false;
}
```

[ ] 1.5 In `gc()` liveCount calculation, add `+ allocatedStrings.size()` to the sum.

[ ] 1.6 Build and run all tests. All 353 must pass.

[ ] 1.7 Add test `test_gc_strings_collected`: create 1000 strings in a loop, verify `allocatedStrings.size()` doesn't grow unbounded.

[ ] 1.8 Add test `test_gc_strings_pointer_equality`: verify `(== "hello" "hello")` is true after forcing GC.

### Phase 2: Parsing builtins (`int`, `float`)

[ ] 2.1 Add `bblInt` C function near `bblStr`:
- 1 arg required. Int→return unchanged. Float→`static_cast<int64_t>`. String→set `errno=0`, `strtoll` with full-consumption check (`end != start && *end == '\0'`), check `errno != ERANGE`. Other types→throw.

[ ] 2.2 Add `bblFloat` C function:
- 1 arg required. Float→return unchanged. Int→`static_cast<double>`. String→set `errno=0`, `strtod` with full-consumption check, check `errno != ERANGE`. Other types→throw.

[ ] 2.3 Register in `addPrint`: `bbl.defn("int", bblInt);` and `bbl.defn("float", bblFloat);`.

[ ] 2.4 Build & test. Add tests for acceptance criteria 4-13 (10 tests). Include `(int -2.7) → -2` negative truncation test.

### Phase 3: String methods

#### 3a: No-arg methods

[ ] 3a.1 Add a static helper `stringNoArgMethod(BblState& bbl, const std::string& data, const std::string& method)` that handles `length`, `upper`, `lower`, `trim`, `trim-left`, `trim-right`. Returns BblValue. Called from both dispatch sites.

[ ] 3a.2 In DotAccess string block and evalList string dispatch, replace inline `length` with calls to the shared helper.

[ ] 3a.3 Build & test. Tests for criteria 29-33 (both field and call forms).

#### 3b: Access (at, slice)

[ ] 3b.1 In evalList string dispatch, add `at`: 1 int arg, throw if `i < 0 || i >= (int64_t)data.size()`, return `intern(string(1, data[i]))`.

[ ] 3b.2 Add `slice`: 1-2 int args, clamp to bounds, return `intern(data.substr(start, end-start))`.

[ ] 3b.3 Build & test. Tests for criteria 14-17.

#### 3c: Search (find, contains, starts-with, ends-with)

[ ] 3c.1 Add `find`: 1-2 args (needle string, optional start int). If start < 0, throw. Return byte position or -1.

[ ] 3c.2 Add `contains`: 1 string arg. Return bool.

[ ] 3c.3 Add `starts-with`, `ends-with`: 1 string arg each. Return bool.

[ ] 3c.4 Build & test. Tests for criteria 18-23.

#### 3d: Transform (replace, split, join)

[ ] 3d.1 Add `replace`: 2 string args (old, new). Empty old→throw. Loop find+replace, advancing by `new.size()` after each replacement (not by 1). Intern result.

[ ] 3d.2 Add `split`: 1 string arg (sep). Empty sep→throw. Build table with 0-based int keys via `allocTable()`. Set `nextIntKey`.

[ ] 3d.3 Add `join`: 1 arg (table or vector). For tables: iterate `0..tbl->nextIntKey - 1` (contiguous int keys, matching push/at). For vectors: iterate all elements via `readVecElem`. Convert each via `valueToString`. Join with receiver string as separator.

[ ] 3d.4 Build & test. Tests for criteria 24-28.

#### 3e: Padding (pad-left, pad-right)

[ ] 3e.1 Add `pad-left`: 1-2 args (width int, optional fill string default " "). Fill must be 1 char. Prepend.

[ ] 3e.2 Add `pad-right`: same but append.

[ ] 3e.3 Build & test. Tests for criteria 34-36.

### Phase 4: `fmt` builtin

[ ] 4.1 Add `bblFmt` C function: variadic, first arg is format string. Scan for `{}` (consume next arg via `valueToString`), `{{`→`{`, `}}`→`}`. Lone `{` not followed by `}` or `{` is an error. Check all args consumed. Intern result.

[ ] 4.2 Register in `addPrint`: `bbl.defn("fmt", bblFmt);`.

[ ] 4.3 Build & test. Tests for criteria 37-41.

### Phase 5: Documentation & acceptance

[ ] 5.1 Update `doc/bbl.md` with new builtins and methods.
[ ] 5.2 Update `doc/spec.md` with string GC and new operations.
[ ] 5.3 Set `doc/features/string.md` status to `done`.
[ ] 5.4 Build and run all tests one final time.
