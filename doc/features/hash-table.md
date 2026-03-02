# Hash Table for BblTable
Status: proposed

## Goal

Replace BblTable's `vector<pair<BblValue, BblValue>>` linear-scan storage
with an open-addressing hash table.  get/set/has/del become O(1) amortized
instead of O(n).  The table_heavy benchmark (1000 entries) goes from O(n²)
to O(n).

## Background

BblTable currently uses a flat vector of key-value pairs.  Every get, set,
has, and del operation scans the entire vector.  This is fine for the
documented sweet spot of < 50 entries but becomes a bottleneck at scale:

- 1000 entries: ~500K comparisons per benchmark run
- The table_heavy benchmark is 2-5x slower than Lua 5.4 because of this

Lua uses a hybrid array+hash table (array part for integer keys 1..n,
hash part for everything else).  For BBL's simpler use case, a single
open-addressing hash table suffices.

## Design

### Data structure

```cpp
struct BblTable {
    struct Entry {
        BblValue key;
        BblValue val;
        bool occupied = false;
    };

    Entry* buckets = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    int64_t nextIntKey = 0;
    bool marked = false;

    // Insertion-order list for iteration (keys, each, at, pop)
    std::vector<BblValue> order;
};
```

### Hash function

For BblValue keys, hash by type tag + content:

```cpp
static size_t hashValue(const BblValue& v) {
    switch (v.type) {
        case BBL::Type::Int:    return std::hash<int64_t>{}(v.intVal);
        case BBL::Type::String: return std::hash<const void*>{}(v.stringVal);
        case BBL::Type::Float:  return std::hash<double>{}(v.floatVal);
        case BBL::Type::Bool:   return v.boolVal ? 1 : 0;
        default:                return 0;
    }
}
```

String keys hash by pointer (strings are interned, so pointer equality
= content equality).  This is O(1), no character scanning.

### Open addressing with linear probing

```cpp
BblValue* get(const BblValue& key) {
    if (count == 0) return nullptr;
    size_t idx = hashValue(key) & (capacity - 1);  // power-of-2 mask
    for (size_t i = 0; i < capacity; i++) {
        Entry& e = buckets[(idx + i) & (capacity - 1)];
        if (!e.occupied) return nullptr;
        if (bblValueKeyEqual(e.key, key)) return &e.val;
    }
    return nullptr;
}
```

Capacity is always a power of 2.  Modulo is a bitmask (no division).
Load factor threshold: grow at 75% (same as Lua).

### Growth

When `count >= capacity * 3/4`, allocate a new array at 2x capacity,
rehash all occupied entries.  Initial capacity: 8 (covers most BBL
tables).  Empty tables allocate nothing (`buckets = nullptr`).

### Insertion order

BBL's `keys` method and `each` iteration return entries in insertion
order.  The `order` vector tracks this.  `del` removes from both the
hash table and the order vector (O(n) for del, but del is rare).

Alternative: use a linked list through the entries for O(1) ordered
iteration.  But the vector is simpler and BBL tables are small.

### API compatibility

The public interface stays identical:
- `get(key)` → O(1) amortized (was O(n))
- `set(key, val)` → O(1) amortized (was O(n))
- `has(key)` → O(1) amortized (was O(n))
- `del(key)` → O(1) hash + O(n) order removal (was O(n))
- `length()` → O(1) (unchanged)
- `at(i)` → O(1) via order vector (unchanged semantics)
- `keys()` → O(n) iteration via order vector (unchanged)
- `push(val)` → O(1) amortized (unchanged)
- `pop()` → O(1) (unchanged)

### Breaking changes

The only direct `entries` access is in:
- `vm.cpp:463`: `tbl->entries[i].second` in the `at` method → use `order[i]`
- `bbl.cpp:2433-2444`: pop method scans for max int key → use `order.back()`
- `bbl.cpp:3966`: message serialization iterates entries → iterate `order`
- `bbl.cpp`: `each` iteration → iterate `order`

All of these are straightforward to migrate.

### GC integration

The hash table buckets contain BblValue keys and values.  GC marking
must scan all occupied buckets (same as scanning the current entries
vector).  No change to GC protocol.

### Memory

Current: `sizeof(pair<BblValue, BblValue>) * n` = 32n bytes for n entries.
New: `sizeof(Entry) * capacity` = 33*capacity bytes (key + val + bool) +
order vector (8n bytes for pointers).  At 75% load factor, capacity ≈
1.33n, so total ≈ 33 * 1.33n + 8n ≈ 52n bytes.  ~60% more memory per
entry, but O(1) access.

For small tables (< 8 entries), the initial 8-bucket allocation (264
bytes) is larger than the current vector approach (~256 bytes for 8
entries).  Negligible difference.

## Estimated complexity

~150 LOC: rewrite BblTable struct and its 4 methods, update ~10 call
sites that access `entries` directly, update GC marking.

## Expected impact

table_heavy benchmark: O(n²) → O(n).  Should match or beat Lua 5.4's
2ms.  No impact on other benchmarks (they don't use large tables).
