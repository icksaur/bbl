# structs

Three separate things that need to work together:
1. **Type descriptor** — field layout, method table.  Lives on `BblState`, global lifetime.
2. **Struct instance** — the binary data (C++ layout).  Value type: copied on assignment.
3. **Member functions** — stored in the type descriptor, NOT in instances.

## syntax — decided

Use `[= Name [struct ...]]` so that `Name` is a normal symbol binding.  `Name` is a
**type value** that can be called as a constructor and used as a composition type.

```bbl
[= Foo [struct [i32 i]]]          // defines type Foo
[= foo [Foo 42]]                   // constructs instance, i = 42
[= Bar [struct [Foo a i32 b]]]     // composing
```

`[Foo 42]` is syntactic sugar for `[construct Foo 42]` — the type value is callable as a constructor.

## member functions — decided

Member functions are defined inline in the struct definition.  They are stored in the
type descriptor.  `this` is an **implicit first argument** of type `Foo`.

```bbl
[= Foo [struct [i32 i]
    [= print-self [fn [this]
        [print this.i]
    ]]
]]

[= foo [Foo 42]]
[foo.print-self]   // calls print-self with foo as this
```

`sizeof foo` = 4 bytes.  Functions are not in the instance.

Member functions can capture variables from the enclosing scope like any other `fn`.
Standard closure rules apply (see memory-model.md).

## the mutation problem

Because struct instances are value types, `this` inside a member function is a **copy**.
Mutations to `this` do not affect the original.

Two patterns to work around this:

**Pattern 1: return the modified struct**
```bbl
[= Foo [struct [i32 n]
    [= inc [fn [this]
        [= this.n [+ this.n 1]]
        this    // return modified copy
    ]]
]]

[= foo [Foo 0]]
[= foo [foo.inc]]   // replace foo with the returned copy
[print foo.n]       // 1
```

**Pattern 2: store the struct in a map or list (holds a reference to the slot)**

Deferred.  For v1, use Pattern 1 (return-and-rebind).  `box` (heap-wrapping for pass-by-reference mutation) is in the backlog.

## returning structs from functions — resolved

Refcounting handles this.  When `fn` returns a struct, its value-type fields are copied
and its reference-type fields (strings, containers) have their refcounts incremented
before the frame is destroyed.  The caller receives a live value.

```bbl
[= make-foo [fn []
    [= Foo [struct [string s]]]
    [= foo [Foo "hello"]]       // "hello" is interned, refcount incremented
    foo                          // returned: struct copied out, string refcount stays alive
]]

[= result [make-foo]]
[print result.s]                 // "hello" is still alive — refcount held by result.s
```

What survives:
- **Type `Foo`**: written to the caller's local scope when `struct` was defined inside the
  function.  This is a problem — see open question below.
- **Instance `foo`**: the struct value is copied into the return slot.
- **String `"hello"`**: interned, lives as long as `BblState`.

## type descriptor lifetime when defined inside a function

If `[= Foo [struct ...]]` is inside a `fn`, where does the type descriptor live?

**Decided: Option A.** Type descriptors are always global.  Defining a struct
inside a function is just a dynamic registration of a new type.  Small memory cost, zero
lifetime complexity.

**Redefinition**: re-registering a struct with an identical layout is a silent no-op.
A different layout is a runtime error.  This means calling a function that defines a struct
multiple times is fine as long as the definition doesn't change.

## struct as refcounted vs value type

Currently structs are value types (copied).  Mutation requires return-and-rebind: `[= foo [foo.inc]]`.

**Decided for v1**: return-and-rebind.  `box` (heap-wrapping for mutable reference semantics) deferred to backlog.

For the serialization use case, value semantics are preferred — vectors of structs work
because elements are inline.  For the scripting use case, `box` may be added later.
