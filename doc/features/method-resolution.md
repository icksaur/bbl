# method resolution

## goal

Specify exactly how the `.` operator resolves field access and method calls at runtime.  This is the single reference for implementers — all other docs defer here for dispatch details.

## the `.` operator

`v.name` is syntactic sugar for `[. v name]`.  `name` is always a compile-time symbol (not a runtime string).  The interpreter evaluates `v`, inspects its type tag, and resolves `name` against that type.

The resolution algorithm depends on the **category** of the left-hand value:

| category | types | what `.` can resolve |
|----------|-------|---------------------|
| struct | BBL-defined structs, C++-registered structs | fields, member functions |
| typed userdata | `File`, `Socket`, etc. | methods only (no fields) |
| built-in container | `vector`, `map`, `list` | built-in methods |
| string | `string` | built-in methods |
| map (special) | `map` | built-in methods **and** key lookup |
| other | integers, floats, bool, null, binary, fn, plain userdata | `.` is a runtime error |

## resolution by type category

### structs

A struct's type descriptor holds two tables:

1. **Field table** — field name → byte offset (from `StructBuilder` or inline `struct` definition).
2. **Method table** — method name → `fn` value (from inline function definitions or `StructBuilder.method()`).

Resolution order:

1. Look up `name` in the **field table**.  If found → field access (read or write).
2. Look up `name` in the **method table**.  If found → method call.
3. Neither → runtime error: `"no field or method 'name' on type TypeName"`.

**Fields win over methods.**  A struct cannot have a field and a method with the same name — this is a registration error caught at struct definition time:

```bbl
// error at definition time: 'x' is already a field
[= Bad [struct [i32 x]
    [= x [fn [this] this.x]]    // error: duplicate name 'x'
]]
```

### typed userdata

Typed userdata has a type descriptor with a **method table** only (no field table — the underlying `void*` is opaque).

Resolution:

1. Look up `name` in the **method table**.  If found → method call.
2. Not found → runtime error: `"no method 'name' on type TypeName"`.

```bbl
[= f [fopen "data.txt"]]
[f.read]       // method table lookup → read
[f.x]          // error: no method 'x' on type File
```

### built-in containers (vector, list)

`vector` and `list` have a fixed built-in method table registered at `BblState` creation.  No user-defined methods.

| type | methods |
|------|---------|
| `vector` | `push`, `pop`, `clear`, `length`, `at` |
| `list` | `push`, `pop`, `clear`, `length`, `at` |

Resolution:

1. Look up `name` in the built-in method table.  If found → method call.
2. Not found → runtime error: `"no method 'name' on vector"` (or `list`).

### map

Map is special — `.` serves double duty as both method dispatch and key lookup.

Resolution order:

1. Look up `name` in the **built-in method table** (`get`, `set`, `delete`, `has`, `keys`, `length`).  If found → method call.
2. Look up `name` as a **string key** in the map's data.  If found → return the value.
3. Key not found → return `null` (same as `[m.get "missing"]`).

**Methods win over keys.**  If a map has a key `"length"`, `m.length` still calls the `length` method.  Use `[m.get "length"]` to access the key:

```bbl
[= m [map "length" 42]]
[print m.length]           // → 1 (method)
[print [m.get "length"]]   // → 42 (key)
```

This is necessary because otherwise inserting a key named `"get"` would make the map's data inaccessible.

### string

String has a built-in method table.  Currently minimal:

| method |
|--------|
| `length` |

(More methods via `addString` — deferred, see backlog.)

Resolution:

1. Look up `name` in the built-in method table.  If found → method call.
2. Not found → runtime error: `"no method 'name' on string"`.

### types where `.` is an error

Using `.` on any of the following is a runtime error:

- integers (`int8`–`int64`, `uint8`–`uint64`)
- floats (`f32`, `f64`)
- `bool`
- `null`
- `binary`
- `fn`
- plain userdata (no type descriptor)

Error message: `"cannot access member 'name' on type TypeName"`.

## implicit `this`

When `.` resolves to a method, the left-hand value is passed as an implicit first argument (`this`).  The caller does not write `this` in the argument list.

```bbl
[file.write "hello"]
// equivalent to: call write(file, "hello")
// but the user writes 1 arg, not 2
```

Arity checking counts **user-visible arguments only** — `this` is excluded.  A method declared as `[fn [this] ...]` has arity 0 from the caller's perspective.

## field access vs method call — how the interpreter tells them apart

After resolving `name`:

- If resolution found a **field** (struct field table hit) and the expression is a bare dot access `v.name` → read the field value from the struct's binary data.
- If resolution found a **field** and the expression is on the left of `=` → write to the field.
- If resolution found a **method** and additional arguments follow → call the method with `v` as `this` plus the provided arguments.
- If resolution found a **method** and no arguments follow → call the method with `v` as `this` and zero user arguments.

A method cannot be read as a value (no first-class method references off an instance).  `v.inc` where `inc` is a method always means "call `inc` on `v`".

## nested dot chains

Dot chains evaluate left-to-right.  Each `.` resolves on the result of the previous `.`:

```bbl
player.pos.x
```

1. Evaluate `player` → map value.
2. `.pos` → map key lookup → returns a vertex struct.
3. `.x` → struct field access → returns an f32.

Each step uses the resolution rules for whatever type the previous step produced.  No special casing — the same algorithm runs at each `.`.

## write through dot chains (place expressions)

`=` treats a dot chain as a place expression:

```bbl
[= player.pos.x 5.0]
```

1. Evaluate `player` → map value.
2. `.pos` → map key lookup → resolves to a location (not a copy).
3. `.x` → struct field → resolves to a writable byte offset.
4. Write `5.0` to that offset.

Each link in the chain resolves to a **pointer into the parent** — the final link performs the write.  See spec.md "place expressions" section.

## type descriptor storage

All type descriptors live in a global `HashMap<String, TypeDescriptor>` on `BblState`.  They are:

- **Never freed** during script execution.
- Keyed by type name (the string passed to `struct`, `TypeBuilder`, or `StructBuilder`).
- Shared across all scopes — defining a struct inside a function registers it globally.

The `BblValue` type tag identifies which descriptor to look up.  For built-in types (`vector`, `map`, `list`, `string`), the descriptors are pre-registered at `BblState` creation.

## error summary

| condition | error message |
|-----------|---------------|
| `.` on a type with no dispatch | `cannot access member 'name' on type TypeName` |
| struct: name not in fields or methods | `no field or method 'name' on type TypeName` |
| typed userdata: name not in methods | `no method 'name' on type TypeName` |
| vector/list/string: name not in methods | `no method 'name' on TypeName` |
| struct: field and method share a name | definition-time error: `duplicate name 'name' in struct TypeName` |
| method called with wrong arity | `arity mismatch: TypeName.name expects N arguments, got M` |
| write to a method name | `cannot assign to method 'name' on type TypeName` |

## open questions

None.
