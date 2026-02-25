# vector

## goal

Contiguous typed storage for value types and structs.  The primary container for serialization — maps directly to C++ `std::vector<T>` so C++ can read vector data as a flat memory buffer with zero conversion.

## type

`vector` is a **container** (reference type, refcounted).  Assignment shares the reference; mutations are visible through all handles.

## construction

`vector` takes a type and zero or more initial elements.  Each element is a bracket expression matching the type's constructor:

```bbl
[= vertex [struct [f32 x f32 y f32 z]]]
[= verts [vector vertex [0 1 0] [1 0 0] [-1 0 0]]]
[= nums [vector int32 1 2 3 4 5]]
```

The type argument is required.  All elements must match the declared type.  This is the one place BBL enforces a type constraint — it's needed to guarantee contiguous memory layout.

## element access

### by index — `at` method

`at` returns a reference to the element.  Writable via place expressions:

```bbl
[print [verts.at 0].x]             // read field of element 0
[= [verts.at 0] [vertex 5 5 5]]   // overwrite element 0
[= [verts.at 0].x 9.0]            // write single field of element 0
```

Out-of-bounds index → runtime error.

### by iteration — `for`

```bbl
[for v verts
    [print v.x " " v.y " " v.z "\n"]
]
```

The loop variable receives a copy (value type) of each element.  Mutating the loop variable does not modify the vector.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `push` | `[verts.push val]` | append element (must match declared type) |
| `pop` | `[verts.pop]` | remove and return last element.  Error if empty. |
| `clear` | `[verts.clear]` | remove all elements, length becomes 0 |
| `length` | `[verts.length]` | number of elements |
| `at` | `[verts.at i]` | element at index (readable and writable) |

## memory layout

Elements are stored contiguously in a single allocation, identical to C++ `std::vector<T>`.  For a `[vector vertex ...]`, the backing buffer is `vertex[]` — three `f32` values packed per element, no per-element tags or headers.

This is what makes vectors useful for serialization: C++ can call `bbl.getVector<vertex>("verts")` and get a pointer to a flat array it can hand directly to a GPU, file writer, or physics engine.

## type constraint

Vectors are the only BBL container that enforces a homogeneous type.  Every `push` and `at`-write checks that the value's type matches the vector's declared element type.  Type mismatch → runtime error.

This is necessary for the contiguous layout guarantee.  A vector of `vertex` must contain only `vertex` values — no tagged union wrapper per element.

## ownership

- Vector is refcounted.  `[= b a]` shares the reference.
- Elements are value types (integers, floats, structs), stored inline by copy.
- Struct elements with reference-type fields: the struct is copied into the vector, refcounts on reference fields are incremented (shallow copy, same as struct assignment).

## C++ API

```cpp
// get a typed pointer to the vector's backing buffer
auto* verts = bbl.getVector<vertex>("player-verts");
// verts->data is vertex*, verts->count is size_t
for (size_t i = 0; i < verts->count; i++) {
    printf("%.1f %.1f %.1f\n", verts->data[i].x, verts->data[i].y, verts->data[i].z);
}
```

## deferred

- **Resize / reserve** — Capacity management deferred to backlog.  Implementation grows like `std::vector` (amortized O(1) push).
- **Slice** — `[verts.slice 0 3]` returning a new vector deferred to backlog.
