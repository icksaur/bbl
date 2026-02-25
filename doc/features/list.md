# list

## goal

Heterogeneous ordered container.  Holds any mix of types — integers, strings, maps, structs, functions, etc.  Backed by `std::vector<BblValue>` internally (tagged values, not contiguous typed memory).

The `list` complements `vector` (homogeneous, typed, contiguous) and `map` (key-value).  Use `list` when you need a sequence of mixed-type values.

## type

`list` is a **container** (reference type, refcounted).  Assignment shares the reference; mutations are visible through all handles.

## construction

`list` takes zero or more initial elements of any type:

```bbl
[= items [list 1 "two" 3.0 true]]
[= empty [list]]
```

No type argument — elements are tagged values.

## element access

### by index — `at` method

`at` returns the element.  Writable via place expressions:

```bbl
[print [items.at 0]]              // → 1
[= [items.at 1] "TWO"]           // overwrite element 1
```

Out-of-bounds index → runtime error.  No negative indexing.

### by iteration — `for`

```bbl
[for item items
    [print item "\n"]
]
```

The loop variable gets the same value as an assignment would: value types are copied, reference types have their refcount incremented (shared).  Mutating a reference-type element obtained from `for` DOES affect the original list entry.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `push` | `[items.push val]` | append element (any type) |
| `pop` | `[items.pop]` | remove and return last element.  Error if empty. |
| `clear` | `[items.clear]` | remove all elements, length becomes 0 |
| `length` | `[items.length]` | number of elements |
| `at` | `[items.at i]` | element at index (readable and writable) |

## memory layout

Elements are stored as `std::vector<BblValue>` — each element is a tagged union (type tag + value).  This is NOT contiguous typed memory like `vector`.  You cannot hand a list's buffer to C++ as a flat array.

## ownership

- List is refcounted.  `[= b a]` shares the reference.
- Values inside the list follow normal rules: value types are copied in; reference types have their refcount incremented.
- Removing an element decrements the refcount of the value.
- Self-referential insertion (`[items.push items]`) is detected and errors at runtime.

## difference from vector

| | `vector` | `list` |
|-|----------|--------|
| homogeneous | yes (typed) | no (any type) |
| contiguous memory | yes (`T[]`) | no (`BblValue[]`) |
| C++ zero-copy | yes (POD structs) | no |
| type argument required | yes | no |
| use case | serialization, GPU data | general-purpose sequences |

## C++ API

```cpp
auto* l = bbl.getList("items");
size_t len = l->length();
BblValue first = l->at(0);
```

## open questions

None.
