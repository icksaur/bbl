# C++ API

## goal

Lua-style embedding API.  A C++ program creates a `BblState`, registers functions and types, executes scripts, and reads results.  The environment persists across multiple `exec()` calls.  `~BblState` runs the GC to completion and frees all memory.

## BblState lifecycle

```cpp
BblState bbl;

// 1. configure: register functions, types, standard libraries
BBL::addStdLib(bbl);
bbl.defn("my-func", my_func);

// 2. register C++ struct types
addVertex(bbl);
addTriangle(bbl);

// 3. execute scripts — environment accumulates
bbl.exec("setup.bbl");       // defines data using registered types
bbl.exec("scene.bbl");       // can use types from setup.bbl

// 4. introspect the environment
auto* verts = bbl.getVector<vertex>("player-verts");
auto* tex = bbl.getBinary("player-texture");

// 5. ~BblState frees all script data, runs GC
```

`exec()` returns `void`.  On success, the root scope contains everything the script defined.  On error, it throws `BBL::Error` — the `BblState` remains valid for cleanup and introspection, but the failed script cannot be resumed.

**Key property**: `exec()` does not reset the environment.  Each script runs in the same root scope.  The second script sees everything the first script defined.

## registering C functions — `bbl.defn()`

```cpp
bbl.defn("name", function_pointer);
```

Binds a C++ function to a name in the BBL environment.  The function is callable from scripts as `(name args...)`.

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
| `bbl->getIntArg(i)` | `int64_t` | read int argument |
| `bbl->getFloatArg(i)` | `double` | read float argument |
| `bbl->getBoolArg(i)` | `bool` | read bool argument |
| `bbl->getStringArg(i)` | `const char*` | read string argument (interned, valid for BblState lifetime) |
| `bbl->getBinaryArg(i)` | `BblBinary*` | read binary argument |
| `bbl->getArg(i)` | `BblValue` | read argument as tagged value |
| `bbl->getUserDataArg(i)` | `void*` | read userdata argument (opaque pointer) |

Reading an argument with the wrong type accessor is a `BBL::Error`.

### return values

| method | description |
|--------|-------------|
| `bbl->pushInt(val)` | push int return value |
| `bbl->pushFloat(val)` | push float return value |
| `bbl->pushBool(val)` | push bool return value |
| `bbl->pushString(str)` | push string (interned automatically) |
| `bbl->pushNull()` | push null return value |
| `bbl->pushBinary(ptr, size)` | push binary from C++ memory (copies into managed buffer) |
| `bbl->pushUserData("TypeName", ptr)` | push typed userdata |

### example: print function

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
    double a = bbl->getFloatArg(0);
    double b = bbl->getFloatArg(1);
    bbl->pushFloat(a + b);
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
    bbl->pushUserData("File", wrap_file(f));
    return 1;
}
```

The backtrace includes the BBL call site where the C function was invoked.

## environment introspection

After `exec()`, the root scope is alive.  The C++ caller can read any variable defined by the script.

### type-safe getters

| method | returns | description |
|--------|---------|-------------|
| `bbl.getInt(name)` | `int64_t` | read int variable |
| `bbl.getFloat(name)` | `double` | read float variable |
| `bbl.getBool(name)` | `bool` | read bool variable |
| `bbl.getString(name)` | `const char*` | read string variable |
| `bbl.getBinary(name)` | `BblBinary*` | read binary variable |
| `bbl.getVector<T>(name)` | `BblVector<T>*` | read typed vector (contiguous `T*` buffer) |
| `bbl.getTable(name)` | `BblTable*` | read table variable |
| `bbl.get(name)` | `BblValue` | read variable as tagged value |

### existence and type checking

| method | returns | description |
|--------|---------|-------------|
| `bbl.has(name)` | `bool` | whether a variable exists in root scope |
| `bbl.getType(name)` | `BBL::Type` | type tag of a variable (`BBL::Type::Null` if absent) |

### setting variables from C++

| method | description |
|--------|-------------|
| `bbl.setInt(name, val)` | define/overwrite an int variable |
| `bbl.setFloat(name, val)` | define/overwrite a float variable |
| `bbl.setString(name, str)` | define/overwrite a string variable (interned) |
| `bbl.setBinary(name, ptr, size)` | define/overwrite a binary variable (copies into managed buffer) |
| `bbl.set(name, BblValue)` | define/overwrite with a tagged value |

This allows C++ to inject data into the environment before or between `exec()` calls:

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.setInt("screen-width", 1920);
bbl.setInt("screen-height", 1080);
bbl.exec("ui-layout.bbl");
```

### iterating the environment

```cpp
bbl.forEach([](const char* name, const BblValue& val) {
    printf("%s: type=%d\n", name, val.type);
});
```

Iterates all variables in the root scope.

## userdata — opaque C++ pointers

An opaque `void*` with a registered type descriptor (methods and optional destructor).  All userdata is typed — register a type via `TypeBuilder`, then push instances from C functions.

### destructor signature

```cpp
typedef void (*BblDestructor)(void* ptr);
```

The destructor receives the raw `void*` pointer.  It is responsible for freeing the underlying resource.  Called by the GC when the object is collected.

```cpp
void file_destroy(void* ptr) {
    auto* f = static_cast<FileWrapper*>(ptr);
    if (f->handle) fclose(f->handle);
    delete f;
}
```

Rules:
- The destructor **must not** call back into the `BblState`.
- The destructor **must not** throw.  If it does, `std::terminate` is called.
- If no destructor is registered, the pointer is abandoned — C++ is responsible for cleanup.

### registration

```cpp
void addFileType(BblState& bbl) {
    BBL::TypeBuilder builder("File");
    builder.method("read", file_read);
    builder.method("write", file_write);
    builder.method("close", file_close);
    builder.destructor(file_destroy);
    bbl.registerType(builder);

    bbl.defn("fopen", bbl_fopen);
}
```

The type descriptor is registered globally on `BblState`.  Instances carry a type tag that maps to this descriptor for method dispatch.

### creating userdata from C functions

```cpp
int bbl_fopen(BblState* bbl) {
    const char* path = bbl->getStringArg(0);
    FILE* f = fopen(path, "r");
    if (!f) throw BBL::Error{"fopen failed: " + std::string(path)};
    bbl->pushUserData("File", new FileWrapper{f});
    return 1;
}
```

## struct registration from C++

Structs are defined exclusively from C++ via `StructBuilder`.  Scripts use them as constructors and access fields via `.`, but cannot define new struct types.

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

After registration, scripts can use the type:

```bbl
(def v (vertex 1.0 2.0 3.0))
(print v.x)
(def verts (vector vertex (vertex 0 1 0) (vertex 1 0 0)))
```

The registered struct has exactly the same binary layout as the C++ struct — `getVector<vertex>()` returns a pointer to data that C++ can use directly.

### StructBuilder API

| method | description |
|--------|-------------|
| `StructBuilder(name, totalSize)` | begin defining a struct with known `sizeof` |
| `field<T>(name, offset)` | add a field at a byte offset — `T` must be a numeric or bool C type |
| `structField(name, offset, typeName)` | add a nested struct field referencing a previously registered struct |

The builder validates at registration time:
- All field offsets + sizes fit within `totalSize`.
- No two fields overlap.
- Field types must be POD (numeric types, bool, other registered structs).  No strings, containers, or functions.

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

`structField()` references a previously registered struct type by name.  The inner struct's fields are accessible via dot: `tri.a.x`.

## BblVector<T>

Returned by `getVector<T>()`.  Provides direct access to the contiguous backing buffer.

```cpp
auto* verts = bbl.getVector<vertex>("player-verts");
vertex* data = verts->data();      // T* — direct pointer to contiguous elements
size_t n = verts->length();        // number of elements

// hand directly to GPU
glBufferData(GL_ARRAY_BUFFER, n * sizeof(vertex), data, GL_STATIC_DRAW);
```

The pointer is valid as long as the `BblState` is alive and the vector is not reallocated.

## BblBinary

Returned by `getBinary()` or `getBinaryArg()`.  Data is always in memory (no lazy loading).

```cpp
auto* tex = bbl.getBinary("player-texture");
size_t len = tex->length();
const uint8_t* data = tex->data();
```

## BblTable

Returned by `getTable()`.  Provides iteration and lookup from C++.

```cpp
auto* t = bbl.getTable("player");
BblValue name = t->get("name");      // get by string key
bool has_hp = t->has("hp");
size_t len = t->length();
```

## standard library — `BBL::addStdLib()`

One call registers the common built-in functions.  Equivalent to Lua's `luaL_openlibs()`.

```cpp
BblState bbl;
BBL::addStdLib(bbl);
bbl.exec("script.bbl");
```

`addStdLib` is a convenience that calls individual library loaders:

| function | what it registers |
|----------|-------------------|
| `BBL::addPrint(bbl)` | `print` — variadic, prints all args to stdout |
| `BBL::addFileIo(bbl)` | `fopen` → `File` type with `read`, `read-bytes`, `write`, `write-bytes`, `close`, `flush` methods; `filebytes` → read a file into a `BblBinary` |
| `BBL::addMath(bbl)` | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sqrt`, `abs`, `floor`, `ceil`, `min`, `max`, `pow`, `log`, `log2`, `log10`, `exp`; constants `pi`, `e` |
| `BBL::addString(bbl)` | string methods — deferred, placeholder for v1 |

See [features/stdlib.md](features/stdlib.md) for full script-side documentation of each module.

Each can be called individually for a minimal runtime:

```cpp
BblState bbl;
BBL::addPrint(bbl);     // only print, nothing else
bbl.exec("script.bbl");
```

### File type (from addFileIo)

The `File` type is a userdata with methods and a destructor:

```bbl
(def f (fopen "out.txt"))
(f.write "hello")
(def contents (f.read))
(f.close)
// f collected by GC eventually — destructor closes if still open
// prefer explicit close for deterministic resource management
```

## BBL::Type enum

```cpp
namespace BBL {
    enum class Type {
        Null,
        Bool,
        Int,       // int64_t
        Float,     // double
        String,
        Binary,
        Fn,
        Vector,
        Table,
        Struct,
        UserData,
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
// ~BblState is safe regardless — GC cleans up all resources
```

## thread safety

None.  A `BblState` must be used from a single thread.  This is a non-goal (see spec).

## open questions

None — all resolved.
