# Bracket Binary Lisp

## Agent Guide
- Read `plan.md` for current work
- Read `doc/spec.md` for language specification and architecture
- Read `doc/memory-model.md` for scope and lifetime design
- Read `doc/structs.md` for struct and member function design
- Read `code-quality.md` before making changes
- Feature designs go in `doc/features/`
- Deferred work and bugs go in `doc/backlog.md`
- Build: `cmake -B build && cmake --build build`
- Test: `./build/bbl_tests`
- Do not start backlog items without asking

---

## usage

Run a script:

```sh
bbl script.bbl
```

Interactive mode:

```sh
bbl
```

The REPL reads bracket expressions and evaluates them.  Multi-line input is automatic — an open `[` without a matching `]` continues to the next line.

---

Bracket Binary Lisp is a bracket-syntax scripting language designed for serializing C++ data structures and binary blobs.  Prefix math, no infix operators.  Embeddable via a Lua-style C++ API.

## data

Value types are copied on assignment:
- `uint8` `uint16` `uint32` `uint64` `int8` `int16` `int32` `int64`
- `f32` `f64`
- `bool`
- `null`
- `userdata` (opaque `void*` pointer)

Reference types are refcounted, shared on assignment:
- `string` (interned)
- `binary` (lazy-loaded byte buffer)
- `fn` (function / closure)

Containers (refcounted):
- `vector` — contiguous typed storage (like C++ `std::vector`)
- `map` — key-value (string/integer keys)
- `list` — heterogeneous ordered collection

## examples

```bbl
[= world "world"]
[= hello [fn []
    [print "Hello, " world "!\n"]
]]
[hello]
```

```bbl
[= vertex [struct [f32 x f32 y f32 z]]]
[= tri [vector vertex [0 1 0]]]
[tri.push [1 0 0]]
[tri.push [-1 0 0]]
```

```bbl
[= texture 0b65536:<65536 bytes of png data>]
```

## control flow

### Loops (prefix math):

```bbl
[= i 0]
[loop [< i 10]
    [print i "\n"]
    [= i [+ i 1]]
]
```

### Conditions (cond is an expression):

```bbl
[= label [cond
    [[== choice 0] "nothing"]
    [[== choice 1] "something"]
    [else          "other"]
]]
```

## closures

```bbl
[= make-adder [fn [n] [fn [x] [+ x n]]]]
[= add5 [make-adder 5]]
[add5 3]   // → 8
```

## C++ API

Lua-style embedding.

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.exec("types.bbl");
bbl.exec("scene.bbl");

auto* verts = bbl.getVector<vertex>("player-verts");
auto* tex = bbl.getBinary("player-texture");
// ~BblState deallocates everything
```

C functions:

```cpp
int my_print(BblState* bbl) {
    int arg = 0;
    while (bbl->hasArg(arg)) {
        switch (bbl->getArgType(arg)) {
            case BBL::Type::String:
                printf("%s", bbl->getStringArg(arg));
                break;
            case BBL::Type::Int32:
                printf("%d", bbl->getInt32Arg(arg));
                break;
        }
        arg++;
    }
    return 0;
}

BblState bbl;
bbl.defn("print", my_print);
bbl.exec("script.bbl");
```

## binary data

Large binary data is lazy-loaded: the lexer skips past raw bytes and only reads them into memory when accessed.

```bbl
[= png-texture 0b4096:<4096 bytes of png>]
[load-texture "character-skin" png-texture]
```

Generate from shell:

```bash
SIZE=$(stat -c%s texture.png)
printf '[= texture 0b%d:' "$SIZE" > scene.bbl
cat texture.png >> scene.bbl
printf ']\n' >> scene.bbl
```
