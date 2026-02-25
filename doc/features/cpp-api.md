# C++ API

## goal

Lua-style embedding API.  A C++ program creates a `BblState`, registers functions, executes scripts, and reads results.  The environment persists across multiple `exec()` calls.  `~BblState` deallocates everything.

## BblState lifecycle

```cpp
BblState bbl;

// 1. configure: register functions, types, standard libraries
BBL::addStdLib(bbl);
bbl.defn("my-func", my_func);

// 2. execute scripts — environment accumulates
bbl.exec("setup.bbl");       // defines types and data
bbl.exec("scene.bbl");       // can use types from setup.bbl
```

`exec()` returns `void`.  On success, the root scope contains everything the script defined.  On error, it throws `BBL::Error` — the `BblState` remains valid for cleanup and introspection (read variables defined before the error), but the failed script cannot be resumed.

// 3. introspect the environment
auto* verts = bbl.getVector<vertex>("player-verts");
auto* tex = bbl.getBinary("player-texture");

// 4. ~BblState deallocates all script data, closes file handles
```

**Key property**: `exec()` does not reset the environment.  Each script runs in the same root scope.  The second script sees everything the first script defined.  This allows multi-file workflows:

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.exec("types.bbl");       // [= vertex [struct [f32 x f32 y f32 z]]]
bbl.exec("scene.bbl");       // uses vertex — it's already defined
auto* v = bbl.getVector<vertex>("player-verts");
```

## registering C functions — `bbl.defn()`

```cpp
bbl.defn("name", function_pointer);
```

Binds a C++ function to a name in the BBL environment.  The function is callable from scripts as `[name args...]`.

### C function signature

```cpp
typedef int (*BblCFunction)(BblState* bbl);
```

A C function receives a pointer to the `BblState`.  It reads arguments by index, performs work, optionally pushes a return value, and returns an integer:

- Return `0`: no return value (void).
- Return `1`: one return value pushed onto the state.

### argument access

| method | returns | description |
|--------|---------|-------------|
| `bbl->argCount()` | `int` | number of arguments passed |
| `bbl->hasArg(i)` | `bool` | whether argument `i` exists |
| `bbl->getArgType(i)` | `BBL::Type` | type tag of argument `i` |
| `bbl->getInt32Arg(i)` | `int32_t` | read int32 argument |
| `bbl->getInt64Arg(i)` | `int64_t` | read int64 argument |
| `bbl->getUint32Arg(i)` | `uint32_t` | read uint32 argument |
| `bbl->getUint64Arg(i)` | `uint64_t` | read uint64 argument |
| `bbl->getF32Arg(i)` | `float` | read f32 argument |
| `bbl->getF64Arg(i)` | `double` | read f64 argument |
| `bbl->getBoolArg(i)` | `bool` | read bool argument |
| `bbl->getStringArg(i)` | `const char*` | read string argument (interned, valid for BblState lifetime) |
| `bbl->getBinaryArg(i)` | `BblBinary*` | read binary argument (lazy-loads on `data()` access) |
| `bbl->getArg(i)` | `BblValue` | read argument as tagged value |
| `bbl->getUserDataArg(i)` | `void*` | read userdata argument (opaque pointer) |

Reading an argument with the wrong type accessor is a `BBL::Error` (e.g. calling `getStringArg()` on an int).

### return values

| method | description |
|--------|-------------|
| `bbl->pushInt32(val)` | push int32 return value |
| `bbl->pushInt64(val)` | push int64 return value |
| `bbl->pushF32(val)` | push f32 return value |
| `bbl->pushF64(val)` | push f64 return value |
| `bbl->pushBool(val)` | push bool return value |
| `bbl->pushString(str)` | push string (interned automatically) |
| `bbl->pushNull()` | push null return value |
| `bbl->pushBinary(ptr, size)` | push binary from C++ memory (copies into managed buffer) |
| `bbl->pushUserData(ptr)` | push userdata (opaque `void*`, no ownership) |

### example: print function

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
            case BBL::Type::F64:
                printf("%f", bbl->getF64Arg(arg));
                break;
            case BBL::Type::Bool:
                printf("%s", bbl->getBoolArg(arg) ? "true" : "false");
                break;
            default:
                printf("<unknown>");
                break;
        }
        arg++;
    }
    return 0;  // no return value
}
```

### example: function with return value

```cpp
int my_add(BblState* bbl) {
    double a = bbl->getF64Arg(0);
    double b = bbl->getF64Arg(1);
    bbl->pushF64(a + b);
    return 1;  // one return value
}
```

### error signaling

C functions signal errors by throwing `BBL::Error`:

```cpp
int my_fopen(BblState* bbl) {
    const char* path = bbl->getStringArg(0);
    FILE* f = fopen(path, "r");
    if (!f) {
        throw BBL::Error{"fopen failed: " + std::string(path)};
    }
    // ... wrap f in a BBL-managed object, push it
    return 1;
}
```

The backtrace includes the BBL call site where the C function was invoked.

## environment introspection

After `exec()`, the root scope is alive.  The C++ caller can read any variable defined by the script.

### type-safe getters

| method | returns | description |
|--------|---------|-------------|
| `bbl.getInt32(name)` | `int32_t` | read int32 variable |
| `bbl.getInt64(name)` | `int64_t` | read int64 variable |
| `bbl.getF32(name)` | `float` | read f32 variable |
| `bbl.getF64(name)` | `double` | read f64 variable |
| `bbl.getBool(name)` | `bool` | read bool variable |
| `bbl.getString(name)` | `const char*` | read string variable |
| `bbl.getBinary(name)` | `BblBinary*` | read binary variable (lazy-loads on `data()`) |
| `bbl.getVector<T>(name)` | `BblVector<T>*` | read typed vector (contiguous `T*` buffer) |
| `bbl.getMap(name)` | `BblMap*` | read map variable |
| `bbl.getList(name)` | `BblList*` | read list variable |
| `bbl.get(name)` | `BblValue` | read variable as tagged value |

### existence and type checking

| method | returns | description |
|--------|---------|-------------|
| `bbl.has(name)` | `bool` | whether a variable exists in root scope |
| `bbl.getType(name)` | `BBL::Type` | type tag of a variable (`BBL::Type::Null` if absent) |

### setting variables from C++

| method | description |
|--------|-------------|
| `bbl.setInt32(name, val)` | define/overwrite an int32 variable |
| `bbl.setString(name, str)` | define/overwrite a string variable (interned) |
| `bbl.setBinary(name, ptr, size)` | define/overwrite a binary variable (copies into managed buffer) |
| `bbl.set(name, BblValue)` | define/overwrite with a tagged value |
| `bbl.setUserData(name, ptr)` | define/overwrite a userdata variable (opaque `void*`) |

This allows C++ to inject data into the environment before or between `exec()` calls:

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.setInt32("screen-width", 1920);
bbl.setInt32("screen-height", 1080);
bbl.exec("ui-layout.bbl");  // script can read screen-width, screen-height
```

### iterating the environment

```cpp
bbl.forEach([](const char* name, const BblValue& val) {
    printf("%s: type=%d\n", name, val.type);
});
```

Iterates all variables in the root scope.  Useful for debugging or serialization round-tripping.

## userdata — opaque C++ pointers

Like Lua's `lightuserdata` and `userdata` combined into one type.  A `void*` value that scripts can store and pass around but cannot dereference.  C++ owns the lifetime.

Two flavors, same BBL type:

1. **Plain userdata** — bare pointer, no methods, no destructor.  Value type (copied on assignment like an integer).  C++ is fully responsible for the pointed-to object's lifetime.

2. **Typed userdata** — pointer with a registered type descriptor (methods and optional destructor).  Reference type (refcounted).  When the refcount hits zero, the destructor runs.  This is how `File`, `Socket`, etc. are implemented.

Both are `BBL::Type::UserData` at the type-tag level.  The difference is whether a type descriptor is attached.

### plain userdata

```cpp
// inject a pointer into the BBL environment
bbl.setUserData("engine", my_engine_ptr);
bbl.exec("script.bbl");
```

```bbl
// script can pass it to C functions but can't inspect it
[render engine player-verts]   // engine is a userdata — opaque to scripts
```

Plain userdata is a **value type** — copied on assignment (it's just a pointer-width integer).  No refcounting, no destructor.

C function access:

```cpp
int my_render(BblState* bbl) {
    void* engine = bbl->getUserDataArg(0);
    auto* verts = bbl->getArg(1);  // BblValue
    // ...
    return 0;
}
```

### typed userdata (methods + destructor)

C++ can register a named type with a method table and optional destructor.  Instances are refcounted — when the last reference drops, the destructor runs.

#### destructor signature

```cpp
typedef void (*BblDestructor)(void* ptr);
```

The destructor receives the raw `void*` pointer that was stored in the userdata.  It is responsible for freeing the underlying resource.  It is called **exactly once**, deterministically, when the refcount reaches zero.

```cpp
void socket_destroy(void* ptr) {
    auto* sock = static_cast<MySocket*>(ptr);
    sock->shutdown();
    delete sock;
}
```

Rules:
- The destructor **must not** call back into the `BblState` (the object may be mid-teardown).
- The destructor **must not** throw.  If it does, `std::terminate` is called.
- If no destructor is registered, the pointer is abandoned — C++ is responsible for cleanup elsewhere (same as plain userdata).

#### registration

```cpp
void addMyType(BblState& bbl) {
    BBL::TypeBuilder builder("Socket");
    builder.method("send", socket_send);
    builder.method("recv", socket_recv);
    builder.method("close", socket_close);
    builder.destructor(socket_destroy);   // called when refcount → 0
    bbl.registerType(builder);

    bbl.defn("connect", socket_connect);  // returns a Socket
}
```

The type descriptor is registered globally on `BblState`.  Instances carry a type tag that maps to this descriptor for method dispatch.  The destructor runs deterministically at refcount zero.

#### creating typed userdata from C functions

C functions create typed userdata by pushing with a type name:

```cpp
int socket_connect(BblState* bbl) {
    auto* sock = new MySocket(...);
    bbl->pushTypedUserData("Socket", sock);
    return 1;
}
```

## struct registration from C++

C++ can define struct types so scripts can construct and use them without re-defining in BBL.  Since C++ has no struct reflection (P2996 targets C++26 but no compiler ships it yet), registration uses a builder pattern with manual field declarations.  `sizeof` and `static_assert` provide safety.

```cpp
struct vertex {
    float x, y, z;
};

void addVertex(BblState& bbl) {
    BBL::StructBuilder builder("vertex", sizeof(vertex));
    builder.field<float>("x", offsetof(vertex, x));
    builder.field<float>("y", offsetof(vertex, y));
    builder.field<float>("z", offsetof(vertex, z));

    // static_assert inside field<T>() verifies:
    // - offsetof + sizeof(T) <= sizeof(vertex)
    // - T maps to a known BBL type

    bbl.registerStruct(builder);
}
```

After registration, scripts can use the type as if it were defined in BBL:

```bbl
[= v [vertex 1.0 2.0 3.0]]
[print v.x]                    // field access works
[= verts [vector vertex [0 1 0] [1 0 0]]]
```

The registered struct has exactly the same binary layout as the C++ struct — `getVector<vertex>()` returns a pointer to data that C++ can use directly.

### StructBuilder API

| method | description |
|--------|-------------|
| `StructBuilder(name, totalSize)` | begin defining a struct with known `sizeof` |
| `field<T>(name, offset)` | add a field at a byte offset — `T` must map to a BBL value type |
| `method(name, fn)` | add a member function (same as TypeBuilder) |

The builder validates at registration time:
- All field offsets + sizes fit within `totalSize`.
- No two fields overlap.
- The last field's `offset + sizeof(field)` does not exceed `totalSize`.  If declared fields don't account for all bytes (due to padding or missed fields), a warning is emitted — helps catch mistakes without requiring the builder to understand C++ alignment rules.

For composed structs:

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

`structField()` references a previously registered struct type by name.  The inner struct's fields are accessible via nested dot: `tri.a.x`.

## BblVector<T>

Returned by `getVector<T>()`.  Provides direct access to the contiguous backing buffer.

```cpp
auto* verts = bbl.getVector<vertex>("player-verts");
vertex* data = verts->data();      // T* — direct pointer to contiguous elements
size_t n = verts->length();        // number of elements

// hand directly to GPU
glBufferData(GL_ARRAY_BUFFER, n * sizeof(vertex), data, GL_STATIC_DRAW);
```

The pointer is valid as long as the `BblState` is alive and the vector is not reallocated (no push from script side while C++ holds the pointer).

## BblBinary

Returned by `getBinary()` or `getBinaryArg()`.  Lazy-loads on access.

```cpp
auto* tex = bbl.getBinary("player-texture");
size_t len = tex->length();            // always available (no load)
const uint8_t* data = tex->data();     // triggers lazy load if needed
```

## BblMap / BblList

Returned by `getMap()` / `getList()`.  Provide iteration and lookup from C++.

```cpp
auto* m = bbl.getMap("player");
BblValue name = m->get("name");      // get by string key
bool has_hp = m->has("hp");

auto* l = bbl.getList("items");
size_t len = l->length();
BblValue first = l->at(0);
```

## standard library — `BBL::addStdLib()`

One call registers the common built-in functions.  Equivalent to Lua's `luaL_openlibs()`.

```cpp
BblState bbl;
BBL::addStdLib(bbl);    // registers print, file I/O, math, etc.
bbl.exec("script.bbl");
```

`addStdLib` is a convenience that calls individual library loaders:

| function | what it registers |
|----------|-------------------|
| `BBL::addPrint(bbl)` | `print` — variadic, prints all args to stdout |
| `BBL::addFileIo(bbl)` | `fopen` → `File` type with `read`, `read-bytes`, `write`, `write-bytes`, `close`, `flush` methods and RAII destructor; `filebytes` → lazy `BblBinary` from an external file |
| `BBL::addMath(bbl)` | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `abs`, `floor`, `ceil`, `min`, `max`, `pow`, `log`, `log2`, `log10`, `exp`; constants `pi`, `e` |
| `BBL::addSocket(bbl)` | `connect` → `Socket` type, `listen` → `ServerSocket` type — TCP client/server with RAII |
| `BBL::addString(bbl)` | string methods — deferred, placeholder for v1 |

See [features/stdlib.md](features/stdlib.md) for full script-side documentation of each module.

Each can be called individually for a minimal runtime:

```cpp
BblState bbl;
BBL::addPrint(bbl);     // only print, nothing else
bbl.exec("script.bbl");
```

### File type (from addFileIo)

The `File` type is a typed userdata with methods and a destructor:

```bbl
[= f [fopen "out.txt"]]
[f.write "hello"]
[= contents [f.read]]
[f.close]
// or: f destroyed at end of scope — ~File flushes and closes automatically
```

`File` is registered via `TypeBuilder` (typed userdata).  When the file's refcount hits zero, the destructor flushes and closes the handle.  This is the RAII pattern — no explicit `close` needed if the scope handles it.

### registering custom typed userdata

See the "typed userdata" section above for the `TypeBuilder` pattern.  `File` and `Socket` are both examples of this pattern.

## BBL::Type enum

```cpp
namespace BBL {
    enum class Type {
        Null,
        Bool,
        Int8, Int16, Int32, Int64,
        Uint8, Uint16, Uint32, Uint64,
        F32, F64,
        String,
        Binary,
        Fn,
        Vector,
        Map,
        List,
        Struct,    // BBL-defined or C++-registered struct types
        UserData,  // void* — plain (value type) or typed (refcounted, with methods/destructor)
    };
}
```

## error handling

All errors throw `BBL::Error`.  See [errors.md](errors.md) for details.

```cpp
try {
    bbl.exec("script.bbl");
} catch (const BBL::Error& e) {
    fprintf(stderr, "bbl failed: %s\n", e.what.c_str());
}
// ~BblState is safe regardless — cleans up all resources
```

After a `BBL::Error`, the `BblState` is still valid for cleanup and introspection, but you cannot resume execution of the failed script.  You can call `exec()` again with a different script if desired.

## thread safety

None.  A `BblState` must be used from a single thread.  This is a non-goal (see spec).

## open questions

None — all resolved.
