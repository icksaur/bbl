# Basic Binary Lisp

Basic Binary Lisp is a scripting language designed for serializing C++ data structures and binary blobs.  Prefix math, no infix operators.  Embeddable via a Lua-style C++ API.  Simple tracing GC.

---

## examples

```bbl
(= world "world")
(= hello (fn ()
    (print "Hello, " world "!\n")
))
(hello)
```

```bbl
// vertex registered from C++ via StructBuilder
(= tri (vector vertex (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(print (tri:at 0).x)
```

```bbl
(= texture 0b65536:<65536 bytes of png data>)
```

## usage

Run a script:

```sh
bbl script.bbl
```

Interactive mode:

```sh
bbl
```

The REPL reads s-expressions and evaluates them.  Multi-line input is automatic — an open `(` without a matching `)` continues to the next line.

---

## install

### from source (any distro)

```sh
git clone https://github.com/carlviedmern/bbl
cd bbl
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build          # installs to /usr/local/bin
```

To choose a different prefix: `cmake -B build -DCMAKE_INSTALL_PREFIX=/usr ...`

### Arch Linux

From the repo directory:

```sh
makepkg -si
```

---

## full documentation

[bbl language](bbl.md)

[C++ API](api.md)

[implementation notes](implementation.md)

## data types

Value types (copied on assignment):
- `int` (64-bit signed integer)
- `float` (64-bit IEEE double)
- `bool`
- `null`
- `struct` (C++-compatible binary layout, POD only)

GC-managed types (shared on assignment):
- `string` (interned, immutable)
- `binary` (raw byte buffer)
- `fn` (function / closure)
- `vector` (contiguous typed storage)
- `table` (heterogeneous key-value container)
- `userdata` (opaque `void*` with type descriptor)

## control flow

### loops

```bbl
(= i 0)
(loop (< i 10)
    (print i "\n")
    (= i (+ i 1))
)
```

### conditionals

```bbl
(= label "other")
(if (== choice 0) (= label "zero"))
(if (== choice 1) (= label "one"))
```

## tables

Heterogeneous key-value container.  String and integer keys.

```bbl
(= player (table "name" "hero" "hp" 100 "alive" true))
(print player.name)
(= player.hp 80)

(= items (table 1 "sword" 2 "shield" 3 "potion"))
(print (items:at 1))
```

## closures

```bbl
(= make-adder (fn (n) (fn (x) (+ x n))))
(= add5 (make-adder 5))
(add5 3)   // 8
```

## C++ API

Lua-style embedding.

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.execfile("types.bbl");
bbl.execfile("scene.bbl");

auto* verts = bbl.getVector<vertex>("player-verts");
auto* tex = bbl.getBinary("player-texture");
// ~BblState frees all script data, runs GC
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
            case BBL::Type::Int:
                printf("%lld", bbl->getIntArg(arg));
                break;
            case BBL::Type::Float:
                printf("%g", bbl->getFloatArg(arg));
                break;
        }
        arg++;
    }
    return 0;
}

BblState bbl;
bbl.defn("print", my_print);
bbl.execfile("script.bbl");
```

## binary data

Binary blob literal: `0b<size>:<raw bytes>`.  The lexer reads all bytes immediately into memory.

```bbl
(= png-texture 0b4096:<4096 bytes of png>)
(load-texture "character-skin" png-texture)
```

Read from file:

```bbl
(= texture (filebytes "texture.png"))
```

Generate from shell:

```bash
SIZE=$(stat -c%s texture.png)
printf '(= texture 0b%d:' "$SIZE" > scene.bbl
cat texture.png >> scene.bbl
printf ')\n' >> scene.bbl
```
