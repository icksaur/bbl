# Method Resolution

## the `.` operator

The `.` operator provides field access and method dispatch.  `v.x` is syntactic sugar for `(. v x)`.  The right-hand side is always an identifier — it is never evaluated as an expression.

Resolution depends on the type of the left-hand value.

## resolution by type

### struct — field access only

Structs have no methods.  The `.` operator on a struct always resolves to a field lookup.

```bbl
(= v (vertex 1.0 2.0 3.0))
(print v.x)          // read field
(= v.x 5.0)        // write field (single-level place expression)
```

The runtime looks up the field name in the struct's `StructBuilder` descriptor.  If the field exists, it reads or writes at the stored byte offset.  If not, runtime error.

For composed structs, chained access reads through nested descriptors:

```bbl
(= tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(print tri.a.x)      // read: triangle descriptor → field "a" → vertex descriptor → field "x"
```

Chained reads are allowed.  Chained writes are not — use an intermediate variable:

```bbl
(= v tri.a)
(= v.x 5.0)
(= tri.a v)
```

### table — methods first, then key access

Tables have registered methods and also support string-key access via `.`.  The runtime checks methods first:

1. Look up the identifier in the table's method table (`get`, `set`, `delete`, `has`, `keys`, `length`, `push`, `pop`, `at`).
2. If found → method dispatch.  The table is passed as the implicit first argument.
3. If not found → string-key access.  Equivalent to `(t.get "name")`.

```bbl
(= player (table "name" "hero" "hp" 100))

// method dispatch — "keys" matches a table method
(= ks (player.keys))

// string-key access — "name" is not a method, so read
(print player.name)           // "hero"
(= player.hp 80)            // write via place expression
```

This means table keys that collide with method names are only accessible via the `get`/`set` methods:

```bbl
(= t (table "length" 42))
(print t.length)              // table method — returns number of entries, not 42
(print (t.get "length"))      // string-key access — returns 42
```

### vector — method dispatch only

Vectors support method calls.  The `.` operator looks up the identifier in the vector method table:

| method | description |
|--------|-------------|
| `push` | append element |
| `pop` | remove and return last element |
| `clear` | remove all elements |
| `length` | number of elements |
| `at` | access element by index |

```bbl
(= verts (vector vertex (vertex 0 1 0)))
(verts.push (vertex 1 0 0))
(print (verts.length))       // 2
(print (verts.at 0).x)       // 0
```

Using `.` with an identifier that is not a registered method is a runtime error.

### string — method dispatch only

Strings support method calls via the `.` operator.

| method | description |
|--------|-------------|
| `length` | number of bytes |

Additional string methods are deferred (see stdlib).

```bbl
(= s "hello")
(print (s.length))           // 5
```

Using `.` with an unknown identifier on a string is a runtime error.

### userdata — method dispatch only

Userdata types have methods registered via `TypeBuilder` in C++.  The `.` operator looks up the identifier in the type descriptor's method table.

```bbl
(= f (fopen "out.txt"))
(f.write "hello")
(f.close)
```

Resolution: type tag → type descriptor (registered on `BblState`) → method table → lookup identifier.  If found, call with the userdata as implicit first argument.  If not found, runtime error.

### binary — method dispatch only

Binaries support a single method via the `.` operator.

| method | description |
|--------|-------------|
| `length` | byte count |

```bbl
(= blob (filebytes "texture.png"))
(print (blob.length))
```

Using `.` with an unknown identifier on a binary is a runtime error.

### other types — error

Using `.` on `int`, `float`, `bool`, `null`, or `fn` is a runtime error.

## dispatch mechanism

Every `BblValue` carries a type tag.  For types with methods (vector, table, string, binary, userdata), the type tag maps to a **type descriptor** stored in a global table on the `BblState`.  The descriptor holds the method table (name → C function pointer).

```
value.type_tag  →  BblState.type_descriptors[tag]  →  descriptor.methods["write"]  →  call
```

For structs, the type tag maps to a **struct descriptor** that holds field layout (name → offset + type), not methods.

Type descriptors are registered at startup and never modified during script execution.

## implicit first argument

When `.` dispatches a method call, the left-hand value is passed as an implicit first argument.  The C function receives it at argument index 0.

```bbl
(f.write "hello")
// equivalent to: call "write" with args [f, "hello"]
```

The C function reads the receiver:

```cpp
int file_write(BblState* bbl) {
    auto* f = static_cast<FileWrapper*>(bbl->getUserDataArg(0));
    const char* text = bbl->getStringArg(1);
    // ...
}
```

## place expression writes via `.`

`set` accepts single-level `.` place expressions:

| pattern | meaning |
|---------|---------|
| `(= v.x 5.0)` | struct field write |
| `(= player.hp 80)` | table string-key write |

For struct fields, the runtime writes directly at the field offset.  For tables, it writes the string key.

Only single-level writes are allowed.  `(= tri.a.x 5.0)` is a compile error.  Use an intermediate variable instead.

## error cases

| condition | error |
|-----------|-------|
| `.` on int, float, bool, null, fn | type error: type has no fields or methods |
| unknown method on binary | runtime error: binary has no method "name" |
| unknown field on struct | runtime error: struct "T" has no field "name" |
| unknown method on vector, string | runtime error: type has no method "name" |
| unknown method on userdata | runtime error: userdata "T" has no method "name" |
| chained write `(= a.b.c val)` | error: only single-level place writes allowed |

## design rationale

- **No vtable pointers in values.**  Type tags are small (fits in a byte) and map to descriptors via a flat array.  No pointer chasing for dispatch.
- **Structs have no methods.**  Keeping structs POD-only guarantees `memcpy` copy semantics and zero-copy `getVector<T>()` from C++.
- **Table method-first resolution.**  Methods are a small fixed set.  Checking methods first avoids the need for a separate method-call syntax.  Key collisions are rare and resolvable via `get`/`set`.
- **Single-level place writes.**  Simpler implementation — no need to track nested lvalue chains.  The intermediate-variable pattern is explicit about mutation.
