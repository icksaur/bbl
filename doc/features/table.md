# Table

## goal

Heterogeneous key-value container.  Replaces both ordered lists and hash maps — one container type for all dynamic collections.  Keys are strings or integers.  Values can be any type.

## type

`table` is a GC-managed container.  Assignment shares the reference; mutations are visible through all handles.

## construction

`table` takes alternating key-value pairs (string-keyed) or sequential values (implicitly integer-indexed starting at 1):

```bbl
// string-keyed
(= player (table "name" "hero" "hp" 100 "alive" true))

// integer-indexed
(= items (table 1 "sword" 2 "shield" 3 "potion"))

// empty table
(= t (table))
```

## string-key access via `.`

The `.` operator on a table provides syntactic sugar for string-key access:

```bbl
(print player.name)         // read — equivalent to (player.get "name")
(= player.hp 80)          // write via place expression
```

If the identifier matches a table method name (see methods below), the method takes priority.  Use `get`/`set` for keys that collide with method names.

## dynamic key access

```bbl
(= key "hp")
(print (player.get key))    // 80
(player.set key 60)
```

## integer-indexed access

`at` provides 0-based positional access to integer-keyed entries:

```bbl
(= items (table 1 "sword" 2 "shield" 3 "potion"))
(print (items.at 0))        // "sword" (first integer key)
(print (items.at 2))        // "potion" (third integer key)
```

## methods

| method | signature | description |
|--------|-----------|-------------|
| `get` | `(t.get key)` | read value by string or integer key.  Returns `null` if absent. |
| `set` | `(t.set key val)` | write value by string or integer key.  Creates key if absent. |
| `delete` | `(t.delete key)` | remove a key-value pair.  No-op if absent. |
| `has` | `(t.has key)` | returns `bool` — whether the key exists. |
| `keys` | `(t.keys)` | returns a table of all keys (integer-indexed). |
| `length` | `(t.length)` | number of key-value pairs. |
| `push` | `(t.push val)` | append value with the next integer key (auto-incrementing). |
| `pop` | `(t.pop)` | remove and return the value at the highest integer key.  Error if no integer keys. |
| `at` | `(t.at i)` | access integer-keyed entry by 0-based position among integer keys.  0 = first integer key, 1 = second, etc. |

## method-first resolution

When `.` is used on a table, methods are checked before string-key access.  The method names are a fixed set: `get`, `set`, `delete`, `has`, `keys`, `length`, `push`, `pop`, `at`.

If a table has a key that collides with a method name, use `get`/`set` to access it:

```bbl
(= t (table "length" 42))
(print t.length)             // method — returns number of entries
(print (t.get "length"))     // string-key — returns 42
```

## iteration

Table keys can be iterated via `keys`:

```bbl
(= player (table "name" "hero" "hp" 100))
(= ks (player.keys))
(= i 0)
(loop (< i (ks.length))
    (= k (ks.at i))
    (print k ": " (player.get k) "\n")
    (= i (+ i 1))
)
```

## ownership

- Table is GC-managed.  `(= b a)` shares the reference.
- Values can be any type — heterogeneous.
- Tables can contain other tables, closures, userdata, etc.
- The GC traces through table entries to find reachable objects.

## C++ API

```cpp
auto* t = bbl.getTable("player");
BblValue name = t->get("name");      // get by string key
bool has_hp = t->has("hp");
size_t len = t->length();
BblValue first = t->at(1);           // integer key access
```

See [cpp-api.md](cpp-api.md) for the full `BblTable` API.

## open questions

None.
