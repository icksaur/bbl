# typeof
Status: done

## Goal

Add `(typeof val)` builtin that returns the type name of any value as a string.  Enables generic functions that inspect argument types at runtime.

## Design

`typeof` is a C function registered via `defn()`.  It takes exactly one argument and returns a string.

```bbl
(typeof 42)          // "int"
(typeof 3.14)        // "float"
(typeof "hello")     // "string"
(typeof true)        // "bool"
(typeof null)        // "null"
(typeof sqrt)        // "fn"
(typeof (table))     // "table"
(typeof (vector int)) // "vector"
```

### Implementation

A single C function `bblTypeof` near the other builtins (`bblStr`, `bblInt`, etc.):

```cpp
static int bblTypeof(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"typeof requires 1 argument"};
    BblValue arg = bbl->getArg(0);
    bbl->pushString(typeName(arg.type).c_str());
    return 1;
}
```

Registered in `addPrint`: `bbl.defn("typeof", bblTypeof);`

The existing `typeName()` function already returns the correct string for every type: `"null"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"binary"`, `"fn"`, `"vector"`, `"table"`, `"struct"`, `"userdata"`.

### Type name strings

| Type | `typeof` result |
|------|-----------------|
| int | `"int"` |
| float | `"float"` |
| string | `"string"` |
| bool | `"bool"` |
| null | `"null"` |
| binary | `"binary"` |
| fn (BBL) | `"fn"` |
| fn (C) | `"fn"` |
| vector | `"vector"` |
| table | `"table"` |
| struct | `"struct"` |
| userdata | `"userdata"` |

Both BBL functions and C functions return `"fn"` — `typeof` reflects the script-level type, not the implementation detail.  Use `(str val)` to see `<fn>` vs `<cfn>` if the distinction matters.

## Considerations

- `typeof` is not a special form — it evaluates its argument before inspecting the type.  This means `(typeof (+ 1 2))` returns `"int"` (the type of `3`), not a representation of the expression.
- The name `typeof` matches JavaScript convention and is clear.  Alternatives considered: `type` (conflicts with common variable names), `type-of` (more verbose, no precedent in BBL naming).
- `typeof` does not distinguish struct types or userdata types by name.  It returns `"struct"` or `"userdata"` for all.  Struct/userdata type introspection can be a future addition if needed.
- Arity is strictly 1.  `(typeof)` and `(typeof 1 2)` both throw.

## Risks

None.  Single C function, 5 lines of code, uses existing infrastructure.

## Acceptance

1. `(typeof 42)` returns `"int"`.
2. `(typeof 3.14)` returns `"float"`.
3. `(typeof "hello")` returns `"string"`.
4. `(typeof true)` returns `"bool"`.
5. `(typeof null)` returns `"null"`.
6. `(typeof sqrt)` returns `"fn"`.
7. `(typeof (fn (x) x))` returns `"fn"`.
8. `(typeof (table))` returns `"table"`.
9. `(typeof (vector int))` returns `"vector"`.
10. `(typeof 0b4:test)` returns `"binary"`.
11. `(typeof (+ 1 2))` returns `"int"` (evaluates before inspecting).
12. `(typeof)` throws (arity error).
13. All existing tests pass.
14. New tests cover all the above.
