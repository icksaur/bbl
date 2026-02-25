# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

## phase 4 — tables and strings

Deliverable: tables work as heterogeneous key-value containers. String interning comparison works. `<`/`>` on strings is a type error.

[ ] 1. **BblTable type** — Add BblTable struct (ordered entries: vector of pair<BblValue,BblValue> for string/int keys, any-type values, plus nextIntKey counter). Add BblTable* to BblValue union + makeTable factory. Add allocTable to BblState, update destructor. Add allocatedTables pool. Add getTable() C++ API method.

[ ] 2. **Table construction & eval** — `table` special form in evalList: alternating key-value pairs. String or int keys only (error otherwise). DotAccess on table: method-first resolution (get/set/delete/has/keys/length/push/pop/at), then string-key fallback. Place expression `(set t.field val)` writes string key on table.

[ ] 3. **Table methods** — In DotAccess call dispatch: get(key), set(key,val), delete(key), has(key), keys(), length(), push(val), pop(), at(i). `keys` returns a new integer-indexed table. `push` auto-increments integer key. `pop` removes highest integer key (error if none). `at` 0-based position among integer keys.

[ ] 4. **String comparison** — `<`/`>`/`<=`/`>=` on strings throws type error. `==`/`!=` already uses pointer equality via interning (verify).

[ ] 5. **Unit tests** — Construct string-keyed table, read via `.`. Integer-indexed table, read via `at`. get/set/delete/has/keys/length. push/pop for integer keys. Method-first resolution (key "length" vs method). getTable C++. Empty table. get missing key → null. delete missing → no error. pop with no int keys → error. Closure shared table capture. String == interned. String < type error.

[ ] 6. **Functional tests** — tables.bbl, strings.bbl. Validate: build, all tests pass.
