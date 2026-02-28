# Code Review -- bbl.cpp

Review date: 2026-02-28
Primary file: `bbl.cpp` (3134 lines), with `bbl.h` (502 lines) and `main.cpp` (208 lines).
Reviewed against the project's `code-quality.md` and `style.md`.

---

## Summary

BBL is a well-structured embeddable Lisp interpreter with a clear domain purpose (serializing C++ data structures and binary blobs). The code is direct, avoids unnecessary abstraction, and has strong test coverage (~3700 lines of tests). The single-file implementation keeps things simple at this scale.

The findings below are organized by the requested review axes. Items marked **[bug]** are likely defects. Items marked **[suggestion]** are improvements.

---

## 1. Security

### 1.1 [bug] Path traversal check is bypassable (lines 2192-2195)

The `execfile` path restriction checks for `".."` as a substring, which rejects legitimate paths containing `..` in filenames (e.g., `notes..txt`) but is also bypassable on some platforms. The same pattern is duplicated in `bblFilebytes` (lines 2919-2922).

```cpp
if (path.find("..") != std::string::npos) {
    throw BBL::Error{"execfile: parent directory traversal not allowed: " + path};
}
```

**Suggestion:** Use `std::filesystem::weakly_canonical` or resolve the path and verify it stays within the allowed root. Deduplicate the path-validation logic into a shared helper -- the current copy-paste between `execfile` and `bblFilebytes` violates the code-quality principle "only one way to do one thing" and "code must be kept in sync."

### 1.2 [suggestion] `fopen` bypasses path restrictions entirely (line 2953)

`bblFopen` calls `fopen(path, mode)` with no path validation at all, even when `allowOpenFilesystem` is false. Any BBL script with access to `fopen` can read or write arbitrary files. This contradicts the sandbox model that `execfile` and `filebytes` attempt to enforce.

### 1.3 [suggestion] No integer overflow protection on arithmetic (lines 1423-1431)

Integer `+`, `-`, `*` on `int64_t` can silently overflow/underflow. This is undefined behavior in C++ for signed integers. Consider checked arithmetic or documenting that overflow behavior is platform-defined.

### 1.4 [suggestion] Left shift of negative value is undefined (line 1557)

```cpp
if (sf == SpecialForm::Shl) return BblValue::makeInt(val << shift);
```

Left-shifting a negative `int64_t` is undefined behavior prior to C++20. Since the project targets C++20 (per CMakeLists.txt), this is well-defined, but it would be worth a brief comment noting the C++20 dependency for this specific behavior.

---

## 2. Performance

### 2.1 [suggestion] BblTable is O(n) for all operations (lines 24-59)

`BblTable::get`, `set`, `has`, and `del` are all linear scans over a `vector<pair<BblValue, BblValue>>`. This is fine for small tables but becomes a bottleneck as tables grow. The `bblValueKeyEqual` function only supports String and Int keys -- other types silently return false, making lookups fail unexpectedly.

For tables used as arrays (sequential integer keys), this is especially wasteful since every access walks the full entry list.

### 2.2 [suggestion] `gatherFreeVars` uses O(n^2) vector membership tests (lines 871-972)

The `contains()` helper (line 871) does a linear scan over vectors for every symbol encountered. `gatherFreeVarsBody` copies the entire `bound` vector by value on every call (line 886). For deeply nested closures or large function bodies, this compounds.

**Suggestion:** Use `std::unordered_set` for `bound` and `freeVars` in the free-variable analysis.

### 2.3 [suggestion] GC copies local `BblValue` just to mark (lines 616-619)

```cpp
BblValue km = k;
BblValue vm = v;
gcMark(km);
gcMark(vm);
```

These copies are unnecessary. `gcMark` only reads the pointer members; the originals could be cast to non-const or `gcMark` could accept `const BblValue&`. This creates needless copies on every GC cycle for every table entry.

### 2.4 [suggestion] Repeated struct field lookup by name (lines 818-828, 1049-1055)

Dot-access on structs does a linear scan of `desc->fields` to find a field by name, every time the field is accessed. The same pattern appears in the `=` assignment path. For hot loops accessing struct fields, this is O(fields) per access.

**Suggestion:** A `std::unordered_map<std::string, size_t>` mapping field names to indices within `StructDesc` would make this O(1).

### 2.5 [suggestion] `tableMethods` static vector recreated conceptually each call (lines 846-851)

The `tableMethods` vector is `static const`, which is fine for initialization, but the membership check is a linear scan of 9 strings. A `static const std::unordered_set<std::string>` would be cleaner and faster if more methods are added.

---

## 3. Maintainability

### 3.1 [suggestion] `evalList` is a 1100-line monolith (lines 1007-2112)

This single function handles all special forms, dot-access method dispatch for every type (vector, string, binary, table, userdata), struct construction, and general function calls. It is the single largest maintainability risk in the codebase.

Per `code-quality.md`: "complexity -- the greatest enemy." Extracting type-specific method dispatch (string methods, table methods, vector methods) into separate functions would make each piece independently testable and readable, without adding abstraction layers.

### 3.2 [suggestion] Duplicated path-resolution logic (lines 2186-2225, 2908-2931)

The path resolution logic (check `allowOpenFilesystem`, resolve relative to `scriptDir`, try `BBL_PATH`) is copy-pasted between `execfile` and `bblFilebytes`. `bblFopen` has none of it. This violates "only one way to do one thing" and creates drift risk.

**Suggestion:** Extract a `resolvePath(const std::string& path)` method on `BblState` that all file operations use.

### 3.3 [suggestion] String method dispatch is a long if-else chain (lines 1746-1941)

There are 14 string methods handled as sequential `if (method == "...")` blocks, totaling ~200 lines. Adding a new string method means inserting into the middle of `evalList`. The same applies to vector methods (~70 lines) and table methods (~100 lines).

**Suggestion:** A method-dispatch table (map from string to function pointer or lambda) would collapse each to a few lines and make adding methods trivial.

### 3.4 [suggestion] `BblValue` union tag (`type`) and `isCFn` flag are redundant information

The `isCFn` flag on `BblValue` is only meaningful when `type == Fn`. This creates an implicit invariant that must be checked at every call site. A cleaner design would be a separate `Type::CFn` enum value, or keeping `Fn` and `CFn` as two distinct types -- removing the possibility of inconsistent state.

### 3.5 [suggestion] `Token` and `AstNode` carry all possible value fields at once

Both `Token` and `AstNode` contain `intVal`, `floatVal`, `boolVal`, `stringVal`, `binaryData`, and `children` simultaneously. Only one subset is valid for any given `type`. This wastes memory and creates implicit coupling between the type tag and which fields are valid.

Per `style.md`: "Use `std::variant`, `std::optional`, `std::span` where they reduce code." A `std::variant` would enforce correctness at compile time and reduce the size of each node.

---

## 4. Reliability

### 4.1 [bug] `writeVecElem` appends then copies back -- breaks on self-assignment (lines 2661-2671)

```cpp
void BblState::writeVecElem(BblVec* vec, size_t i, const BblValue& val) {
    // ...
    size_t off = i * vec->elemSize;
    packValue(vec, val);               // may realloc vec->data
    uint8_t* dst = vec->data.data() + off;
    uint8_t* src = vec->data.data() + vec->data.size() - vec->elemSize;
    memcpy(dst, src, vec->elemSize);
    vec->data.resize(vec->data.size() - vec->elemSize);
}
```

This appends the value to the end of the vector, then copies it to the target index, then truncates. If `val` references data *within* the same vector (e.g., a struct value read from another index), the `packValue` realloc could invalidate the source data before it's read. More fundamentally, this is an unnecessarily complex way to write to an index -- just write directly to the offset.

### 4.2 [bug] `each` loop length is computed once but container can be mutated (lines 1163-1193)

```cpp
BblValue container = eval(node.children[2], scope);
int64_t len = static_cast<int64_t>(container.vectorVal->length());
// ...
for (int64_t idx = 0; idx < len; idx++) { ... }
```

The length is captured before the loop starts, but the loop body can mutate the container (push, pop, resize). If the body grows the container, entries are silently skipped. If the body shrinks it, `readVecElem` will throw an out-of-bounds error. This is a design choice worth documenting or validating.

### 4.3 [bug] `vector.at` and `readVecElem` accept negative indices without checking (line 1707)

```cpp
return readVecElem(vec, static_cast<size_t>(idx.intVal));
```

A negative `int64_t` cast to `size_t` becomes a very large number. `readVecElem` then checks `i >= vec->length()` which will throw, but the error message will show a nonsensical huge index rather than saying "negative index." The same applies to `vector.set`, dot-access integer indices, and `string.at` (which does check for negative, showing inconsistency).

### 4.4 [suggestion] `exec` special form uses a detached scope (lines 1294-1298)

```cpp
BblScope execScope;
BblValue result = BblValue::makeNull();
for (auto& n : nodes) {
    result = eval(n, execScope);
}
```

The `execScope` has no parent, so `exec`-ed code cannot see any enclosing bindings. But `execScope` is also not registered in `activeScopes`, so GC cycles during `exec` evaluation may collect objects that `execScope` references. This is a potential use-after-free.

### 4.5 [suggestion] `BblScope::set` in flat mode throws on any non-slotted symbol (lines 98-107)

When a scope is in flat mode (has `slotMap`), `set` only checks the slot map and local `bindings`. It never walks the parent chain. This means assigning to a variable defined in an outer scope from within a flat-mode function scope will throw "undefined symbol" instead of finding the outer binding. This appears intentional for closure semantics but differs from the non-flat path, which walks parents.

### 4.6 [suggestion] No stack depth limit

Recursive BBL functions add frames to `activeScopes` and C++ call stack without limit. A deeply recursive BBL program will crash with a stack overflow rather than a clean error message.

---

## 5. Modern C++ Opportunities

### 5.1 [suggestion] Raw `new`/`delete` throughout the GC allocator

`style.md` says: "Avoid raw `new`/`delete` outside of the GC allocation path." The GC path *is* the exemption, but the destructor (lines 487-512) has seven manually-written `for` loops calling `delete`. This is repetitive and error-prone.

**Suggestion:** Use `std::unique_ptr` in the allocation vectors (`allocatedStrings`, etc.). The vectors would then free their contents automatically, and the GC sweep could use `std::erase_if` with a predicate. The destructor becomes trivial or deleted.

### 5.2 [suggestion] GC sweep is copy-pasted seven times (lines 649-719)

The sweep logic (partition, delete, erase, unmark) is identical for every type except for minor variations (string intern table cleanup, userdata destructor calls). This is a strong candidate for a template:

```cpp
template<typename T, typename CleanupFn = std::identity>
void sweepPool(std::vector<T*>& pool, CleanupFn cleanup = {}) {
    auto mid = std::partition(pool.begin(), pool.end(),
                              [](T* obj) { return obj->marked; });
    for (auto it = mid; it != pool.end(); ++it) {
        cleanup(*it);
        delete *it;
    }
    pool.erase(mid, pool.end());
    for (auto* obj : pool) obj->marked = false;
}
```

### 5.3 [suggestion] `std::string_view` underused

`style.md` recommends `std::string_view` for non-owning string references. Many functions take `const std::string&` where they never store the string -- `has()`, `getType()`, `get()`, all the getters and setters, `resolveSymbol()`, `contains()`, field-name lookups. Switching to `std::string_view` avoids implicit allocations when callers pass string literals.

### 5.4 [suggestion] `static_cast<size_t>(idx.intVal)` repeated without validation

This pattern appears at least 10 times. A small helper would centralize the negative-index check and cast:

```cpp
size_t toIndex(int64_t val, size_t length, const char* context) {
    if (val < 0 || static_cast<size_t>(val) >= length) {
        throw BBL::Error{std::string(context) + ": index " + std::to_string(val)
                         + " out of bounds (length " + std::to_string(length) + ")"};
    }
    return static_cast<size_t>(val);
}
```

### 5.5 [suggestion] `[[nodiscard]]` on factory methods

The `BblValue::makeInt`, `makeFloat`, etc. static factories should be `[[nodiscard]]` to catch accidental dropped returns. Same for `allocTable`, `allocFn`, `intern`, etc.

### 5.6 [suggestion] Structured bindings could simplify capture iteration

Several loops iterate over `std::pair` with explicit `.first`/`.second` access. The code already uses structured bindings in most places (e.g., `for (auto& [k, v] : entries)`) but a few spots still use iterator-style access (e.g., line 2023: `tbl->entries[maxIdx].second`).

### 5.7 [suggestion] `constexpr` for special-form lookup table

The `lookupSpecialForm` function builds a `static const std::unordered_map` on first call. Since C++20 is available, a `constexpr` sorted array with binary search or a perfect hash would be evaluated at compile time and avoid the runtime initialization overhead.

---

## 6. Code Quality (per code-quality.md)

### Strengths

- **Minimal code / less is more:** The entire interpreter is three files. No unnecessary abstraction layers.
- **Correct by design:** Strong type checking at the BBL language level -- every operation validates its operand types before executing.
- **Descriptive names:** `BblState`, `BblScope`, `evalList`, `readField`, `writeField`, `packValue` -- all self-explanatory.
- **Unit tests:** 3700+ lines of tests covering lexer, parser, eval, GC, closures, structs, vectors, tables, file I/O, and edge cases.
- **Separation of concerns:** Lexer, parser, evaluator, GC, and stdlib are cleanly separated within the file.

### Areas for Improvement

- **Coupling:** `evalList` is coupled to every type's method set. Adding a string method requires modifying the core evaluator. A dispatch-table approach would decouple method registration from evaluation.
- **Code must be kept in sync:** Path validation is duplicated. Negative-index checking is inconsistent across types.
- **Mutable global-like state:** `BblState` is a god-object with 20+ public fields that any C function can mutate freely (e.g., directly writing `bbl->returnValue` and `bbl->hasReturn` in `bblFilebytes` at line 2939 instead of using `pushBinary`). This creates implicit contracts.

---

## Prioritized Recommendations

1. **Fix `fopen` path validation bypass** -- security gap, straightforward fix
2. **Fix `exec` scope not registered in `activeScopes`** -- potential use-after-free
3. **Add negative-index validation** -- consistent bounds checking across all indexing operations
4. **Extract path resolution into shared helper** -- eliminate duplication, fix consistency
5. **Break `evalList` into smaller dispatch functions** -- biggest maintainability win
6. **Template the GC sweep** -- remove 70 lines of near-identical code
7. **Add stack depth limit** -- prevent C++ stack overflow from recursive scripts
