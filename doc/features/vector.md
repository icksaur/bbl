# Vector

## goal

Contiguous typed storage for value types and structs.  The primary container for serialization — maps directly to C++ `std::vector<T>` so C++ can read vector data as a flat memory buffer with zero conversion.

## type

`vector` is a GC-managed container.  Assignment shares the reference; mutations are visible through all handles.

## construction

`vector` takes a type and zero or more initial elements:

```bbl
(def verts (vector vertex (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(def nums (vector int 1 2 3 4 5))
```

The type argument is required.  All elements must match the declared type.  This is the one place BBL enforces a type constraint — it's needed to guarantee contiguous memory layout.

Allowed element types: numeric types (`int`, `float`), `bool`, and registered structs.  All must be value types (POD).

## element access

### by index — `at` method

`at` returns the element at a given index.  Writable via place expressions (single-level):

```bbl
(print (verts.at 0).x)                    // read field of element 0
(set (verts.at 0) (vertex 5 5 5))         // overwrite element 0
```

Out-of-bounds index → runtime error.

### by iteration — `loop` with `at` and `length`

```bbl
(def i 0)
(loop (< i (verts.length))
    (def v (verts.at i))
    (print v.x " " v.y " " v.z "\n")
    (set i (+ i 1))
)
```

The variable receives a copy (value type) of each element.  Mutating the local copy does not modify the vector.

## methods

| method | signature | description |
|--------|-----------|-------------|
| `push` | `(verts.push val)` | append element (must match declared type) |
| `pop` | `(verts.pop)` | remove and return last element.  Error if empty. |
| `clear` | `(verts.clear)` | remove all elements, length becomes 0 |
| `length` | `(verts.length)` | number of elements |
| `at` | `(verts.at i)` | element at index (readable and writable via place expression) |

## memory layout

Elements are stored contiguously in a single allocation, identical to C++ `std::vector<T>`.  For a `(vector vertex ...)`, the backing buffer is `vertex[]` — three `float32` values packed per element, no per-element tags or headers.

This is what makes vectors useful for serialization: C++ can call `bbl.getVector<vertex>("verts")` and get a pointer to a flat array it can hand directly to a GPU, file writer, or physics engine.

## type constraint

Vectors are the only BBL container that enforces a homogeneous type.  Every `push` and `at`-write checks that the value's type matches the vector's declared element type.  Type mismatch → runtime error.

This is necessary for the contiguous layout guarantee.  A vector of `vertex` must contain only `vertex` values — no tagged union wrapper per element.

## ownership

- Vector is GC-managed.  `(def b a)` shares the reference.
- Elements are value types (integers, floats, structs), stored inline by copy.
- Struct elements are POD only — no GC references inside struct elements.

## C++ API

```cpp
auto* verts = bbl.getVector<vertex>("player-verts");
vertex* data = verts->data();      // T* — direct pointer to contiguous elements
size_t n = verts->length();

// hand directly to GPU
glBufferData(GL_ARRAY_BUFFER, n * sizeof(vertex), data, GL_STATIC_DRAW);
```

The pointer is valid as long as the `BblState` is alive and the vector is not reallocated.

## deferred

- **Resize / reserve** — Capacity management deferred to backlog.  Implementation grows like `std::vector` (amortized O(1) push).
- **Slice** — `(verts.slice 0 3)` returning a new vector deferred to backlog.

## open questions

None.
