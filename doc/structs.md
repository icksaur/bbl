# Structs

## overview

Structs are **value types** with C++-compatible binary layout.  They are defined exclusively from C++ via `StructBuilder` — there is no script-level keyword for defining structs.

Three things work together:

1. **Struct descriptor** — field layout (name → offset + type).  Registered on `BblState`, global lifetime.
2. **Struct instance** — the binary data (matches the C++ struct layout exactly).  Value type: copied on assignment.
3. **Constructor** — after registration, the struct type name is available in scripts as a callable constructor.

## POD only

Struct fields must be value types: numeric types (`int`, `float`, and narrow C types like `float32`, `int32`, etc.), `bool`, and other registered structs.

No strings, containers, functions, binaries, or userdata in struct fields.  This guarantees:

- `memcpy` copy semantics — no GC references to trace inside structs.
- Zero-copy `getVector<T>()` from C++ — the data pointer can be handed directly to a GPU or serializer.
- C++-identical binary layout — `sizeof`, `offsetof`, and alignment match exactly.

## C++ registration

```cpp
struct vertex {
    float x, y, z;
};

void addVertex(BblState& bbl) {
    BBL::StructBuilder builder("vertex", sizeof(vertex));
    builder.field<float>("x", offsetof(vertex, x));
    builder.field<float>("y", offsetof(vertex, y));
    builder.field<float>("z", offsetof(vertex, z));
    bbl.registerStruct(builder);
}
```

### StructBuilder API

| method | description |
|--------|-------------|
| `StructBuilder(name, totalSize)` | begin defining a struct with known `sizeof` |
| `field<T>(name, offset)` | add a field at a byte offset — `T` must be a numeric or bool C type |
| `structField(name, offset, typeName)` | add a nested struct field referencing a previously registered struct |

Validation at registration time:

- All field offsets + sizes fit within `totalSize`.
- No two fields overlap.
- Field types are POD only.
- `structField` references must name a previously registered struct type.

### composed structs

```cpp
struct triangle {
    vertex a, b, c;
};

void addTriangle(BblState& bbl) {
    BBL::StructBuilder builder("triangle", sizeof(triangle));
    builder.structField("a", offsetof(triangle, a), "vertex");
    builder.structField("b", offsetof(triangle, b), "vertex");
    builder.structField("c", offsetof(triangle, c), "vertex");
    bbl.registerStruct(builder);
}
```

The inner struct's fields are accessible via dot chaining: `tri.a.x`.

## script usage

### construction

After registration, the type name is a callable constructor in scripts:

```bbl
(def v (vertex 1.0 2.0 3.0))
```

Arguments are positional, matching the order fields were registered.  The runtime validates argument count and types.

### field access

Read via `.`:

```bbl
(print v.x)          // 1.0
```

Write via `set` (single-level place expression):

```bbl
(set v.x 5.0)
```

### composition

```bbl
(def tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(print tri.a.x)      // read through nested struct — allowed
```

Chained reads work.  Chained writes do not — use an intermediate variable:

```bbl
(def v tri.a)
(set v.x 5.0)
(set tri.a v)
```

### vectors of structs

```bbl
(def verts (vector vertex
    (vertex 0 1 0)
    (vertex 1 0 0)
    (vertex -1 0 0)
))
(print (verts.at 0).x)
```

Because structs are POD, the vector's backing buffer is a contiguous `T*` that C++ can read directly via `getVector<T>()`.

## copy semantics

Structs are value types.  Assignment copies the bytes:

```bbl
(def a (vertex 1.0 2.0 3.0))
(def b a)             // b is an independent copy
(set b.x 99.0)
(print a.x)           // 1.0 — a is unchanged
```

Passing a struct to a function copies it.  Returning a struct from a function copies it.  No GC involvement — structs are inline values like `int` and `float`.

## no member functions

Structs have no methods.  The `.` operator on a struct always resolves to field access.  If you need operations on struct data, define standalone functions:

```bbl
(def vertex-length (fn (v)
    (sqrt (+ (* v.x v.x) (* v.y v.y) (* v.z v.z)))
))
(print (vertex-length (vertex 3.0 4.0 0.0)))   // 5.0
```

This keeps structs simple and POD — no method table, no dispatch overhead.

## type descriptor lifetime

Struct descriptors are registered on the `BblState` and live for its entire lifetime.  They are never freed during script execution.  `~BblState` frees all descriptors.

Re-registering a struct with an identical layout is a silent no-op.  A different layout is a runtime error.

## open questions

None.
