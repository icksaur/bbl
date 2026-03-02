# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

## legend

[ ] incomplete
[x] complete

---

## overview

Replace BblTable's linear-scan vector with an open-addressing hash table
per `doc/features/hash-table.md`.  ~150 LOC.

Build: `cmake --build build`
Test: `./build/bbl_tests && ./tests/run_functional.sh ./build/bbl`

---

## steps

[ ] In `bbl.h`, replace `BblTable` struct:
    - Remove `std::vector<std::pair<BblValue, BblValue>> entries`.
    - Add `struct Entry { BblValue key; BblValue val; bool occupied = false; }`.
    - Add `Entry* buckets = nullptr; size_t capacity = 0; size_t count = 0;`.
    - Add `std::vector<BblValue> insertionKeys;` for ordered iteration.
    - Keep `int64_t nextIntKey = 0; bool marked = false;`.
    - Keep `length()` returning `count`.
    - Add destructor `~BblTable() { delete[] buckets; }`.
    - Keep get/set/has/del signatures unchanged.

[ ] In `bbl.cpp`, rewrite BblTable methods:
    - Add `hashValue(const BblValue&)` static function.
    - Add `findEntry(Entry*, size_t cap, const BblValue&)` helper.
    - Add `growTable(BblTable*)` helper (double capacity, rehash).
    - `get()`: hash+probe, return value or NotFound.
    - `set()`: hash+probe, insert or update. Grow at 75% load. Track insertionKeys.
    - `has()`: hash+probe, return bool.
    - `del()`: hash+probe, mark tombstone (set key to null, keep occupied for probing),
      remove from insertionKeys.

[ ] Update all `entries` access sites:
    - `vm.cpp`: table method "at" → use `insertionKeys[i]` + `get()`.
    - `vm.cpp`: table method "pop" → remove last from `insertionKeys` + `del()`.
    - `vm.cpp`: table method "keys" → iterate `insertionKeys`.
    - `vm.cpp`: table method "push" → use `set()` (unchanged).
    - `vm.cpp`: OP_TABLE handler → use `set()` (unchanged).
    - `bbl.cpp`: evalTableMethod "pop" → same pattern.
    - `bbl.cpp`: evalTableMethod "at" → same pattern.
    - `bbl.cpp`: evalTableMethod "keys" → iterate `insertionKeys`.
    - `bbl.cpp`: serialization in `serializeMessage` → iterate `insertionKeys`.
    - `bbl.cpp`: `each` iteration → iterate via `insertionKeys` + `get()`.

[ ] Update GC marking: scan `buckets[0..capacity)` for occupied entries,
    mark keys and values (same as scanning entries vector).

[ ] Build and run all 780 tests.

[ ] Build release, run table_heavy benchmark vs Lua 5.4.

