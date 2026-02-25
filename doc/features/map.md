# map

## goal

Key-value container with string-key fast path and dynamic-key access.  Serves as the general-purpose "object" or "table" type for unstructured data.

## type

`map` is a **container** (reference type, refcounted).  Assignment shares the reference; mutations are visible through all handles.

## construction

Alternating key-value pairs.  Keys can be any hashable value (strings and integers).  Values can be any type.

```bbl
[= player [map "name" "hero" "hp" 100 "alive" true]]
[= empty [map]]
```

## access

### string keys — dot syntax

The common case.  `.` on a map resolves the right-hand name as a string key.  Read and write:

```bbl
[print player.name]     // → "hero"
[= player.hp 80]        // set field
```

This reuses the same `.` syntax as struct fields.  The interpreter checks the type tag — if it's a map, `.` does a key lookup; if it's a struct, `.` does a field offset.

### dynamic keys — `get` / `set` methods

When the key is in a variable or computed at runtime:

```bbl
[= key "hp"]
[print [player.get key]]   // → 80
[player.set key 60]
```

`get` with a missing key returns `null`.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `get` | `[m.get key]` | return value for key, or `null` |
| `set` | `[m.set key val]` | insert or overwrite |
| `delete` | `[m.delete key]` | remove key-value pair.  No-op if key is absent. |
| `has` | `[m.has key]` | `bool` — whether key exists |
| `keys` | `[m.keys]` | return a `list` of all keys |
| `length` | `[m.length]` | number of key-value pairs |

### method/key name collision

When a map key has the same name as a built-in method (e.g. a key named `"length"` or `"get"`), the **method wins** on dot access.  Use `.get` for dynamic access to such keys:

```bbl
[= m [map "length" 42]]
[print m.length]           // → 1 (calls the length method, not the key)
[print [m.get "length"]]   // → 42 (uses .get to access the key)
```

## iteration

Via `keys` and `for`:

```bbl
[= player [map "name" "hero" "hp" 100]]
[for k [player.keys]
    [print k ": " [player.get k] "\n"]
]
```

Key order is insertion order (like Python 3.7+ dict, Lua 5.x table iteration is unspecified but insertion order is simplest to implement and least surprising).

## C++ representation

Internally a hash map.  Candidate: `std::unordered_map<BblValue, BblValue>` with a hash that dispatches on type tag.  String keys use the interned pointer for O(1) hashing.  Integer keys hash the integer value directly.

## ownership

- Map is refcounted.  `[= b a]` where `a` is a map increments the refcount — both names point to the same map.
- Values inside the map follow normal rules: value types are copied in; reference types have their refcount incremented.
- Removing a key decrements the refcount of both the key and the value.

## nested dot access

Dot chains left-to-right across type boundaries:

```bbl
[= player [map "name" "hero" "pos" [vertex 1.0 2.0 3.0]]]
[print player.pos.x]    // map lookup "pos" → vertex, then field access .x
```

Each `.` resolves on the result of the previous `.`.  No special casing — the interpreter evaluates `player.pos` (map get), gets a vertex, then evaluates `.x` on that (struct field).
