# BBL C++ API Reference

Complete reference for embedding BBL in C++ programs.  Header: `bbl.h`.

---

## Quick Start

```cpp
#include "bbl.h"

int main() {
    BblState bbl;
    BBL::addStdLib(bbl);

    bbl.execfile("setup.bbl");
    bbl.execfile("scene.bbl");

    auto* verts = bbl.getVectorData<vertex>("player-verts");
    size_t count = bbl.getVectorLength<vertex>("player-verts");
    auto* tex = bbl.getBinary("player-texture");

    // use verts, tex for GPU upload, serialization, etc.
    // ~BblState frees all script data, runs GC
}
```

---

## BblState Lifecycle

```cpp
BblState bbl;           // create interpreter state
// ... use ...
// ~BblState() cleans up: runs GC, frees intern table, calls userdata destructors
```

`BblState` is move-only (no copy).  One state per interpreter instance.  Owns
all scopes, the GC heap, type descriptors, and the string intern table.

---

## Execution

### exec

```cpp
bbl.exec("(= x 10)");
bbl.exec("(print x)");     // sees x from previous call
```

Execute a string of BBL code.  From C++, accumulates into the existing root
scope -- definitions persist across calls.

### execExpr

```cpp
BblValue result = bbl.execExpr("(+ 1 2)");
// result.type == BBL::Type::Int, result.intVal == 3
```

Execute a string and return the result as a `BblValue`.

### execfile

```cpp
bbl.execfile("setup.bbl");
bbl.execfile("scene.bbl");    // sees definitions from setup.bbl
```

Load and execute a `.bbl` file.  Accumulates into root scope.  Path is resolved
relative to the current working directory (or `scriptDir` if set).

### Script Context

```cpp
bbl.currentFile = "scene.bbl";                     // shown in error backtraces
bbl.scriptDir = std::filesystem::path("scripts/"); // base for relative paths
```

---

## C Function Registration

### Signature

```cpp
typedef int (*BblCFunction)(BblState* bbl);
```

All C functions callable from BBL follow this signature.  Read arguments by
index, optionally push a return value, return 0 (void) or 1 (one value pushed).

### Registration

```cpp
bbl.defn("my-add", myAdd);
```

After registration, the function is callable from script as `(my-add 1 2)`.

### Reading Arguments

| Method                        | Returns        | Notes                         |
|-------------------------------|----------------|-------------------------------|
| `bbl->argCount()`             | `int`          | Total argument count          |
| `bbl->hasArg(i)`              | `bool`         | Check if argument exists      |
| `bbl->getArgType(i)`          | `BBL::Type`    | Type tag of argument          |
| `bbl->getIntArg(i)`           | `int64_t`      |                               |
| `bbl->getFloatArg(i)`         | `double`       |                               |
| `bbl->getBoolArg(i)`          | `bool`         |                               |
| `bbl->getStringArg(i)`        | `const char*`  | Pointer to interned string    |
| `bbl->getBinaryArg(i)`        | `BblBinary*`   |                               |
| `bbl->getArg(i)`              | `BblValue`     | Generic access                |

### Pushing Return Values

| Method                             | Pushes         |
|------------------------------------|----------------|
| `bbl->pushInt(int64_t)`            | Int            |
| `bbl->pushFloat(double)`           | Float          |
| `bbl->pushBool(bool)`             | Bool           |
| `bbl->pushString(const char*)`     | String         |
| `bbl->pushNull()`                  | Null           |
| `bbl->pushBinary(uint8_t*, size)`  | Binary         |
| `bbl->pushUserData(typeName, ptr)` | UserData       |

### Example

```cpp
static int myAdd(BblState* bbl) {
    int64_t a = bbl->getIntArg(0);
    int64_t b = bbl->getIntArg(1);
    bbl->pushInt(a + b);
    return 1;  // one return value
}

static int myPrint(BblState* bbl) {
    int arg = 0;
    while (bbl->hasArg(arg)) {
        switch (bbl->getArgType(arg)) {
            case BBL::Type::String:
                printf("%s", bbl->getStringArg(arg));
                break;
            case BBL::Type::Int:
                printf("%lld", (long long)bbl->getIntArg(arg));
                break;
            case BBL::Type::Float:
                printf("%g", bbl->getFloatArg(arg));
                break;
            default:
                break;
        }
        arg++;
    }
    return 0;  // void
}

bbl.defn("my-add", myAdd);
bbl.defn("my-print", myPrint);
```

---

## Environment Introspection

### Reading Variables

| Method                      | Returns        | Notes                            |
|-----------------------------|----------------|----------------------------------|
| `bbl.has(name)`             | `bool`         | Variable exists in root scope    |
| `bbl.getType(name)`         | `BBL::Type`    | Type tag                         |
| `bbl.get(name)`             | `BblValue`     | Generic access                   |
| `bbl.getInt(name)`          | `int64_t`      |                                  |
| `bbl.getFloat(name)`        | `double`       |                                  |
| `bbl.getBool(name)`         | `bool`         |                                  |
| `bbl.getString(name)`       | `const char*`  | Pointer to interned string data  |
| `bbl.getTable(name)`        | `BblTable*`    |                                  |
| `bbl.getBinary(name)`       | `BblBinary*`   |                                  |

### Writing Variables

| Method                               | Sets           |
|--------------------------------------|----------------|
| `bbl.setInt(name, int64_t)`          | Int            |
| `bbl.setFloat(name, double)`         | Float          |
| `bbl.setString(name, const char*)`   | String         |
| `bbl.setBinary(name, uint8_t*, sz)`  | Binary         |
| `bbl.set(name, BblValue)`           | Any value      |

### Example

```cpp
bbl.exec("(= score 42)");

if (bbl.has("score")) {
    int64_t score = bbl.getInt("score");
    printf("Score: %lld\n", (long long)score);
}

bbl.setInt("level", 5);
bbl.exec("(print level)");  // prints 5
```

---

## Type System

### BBL::Type Enum

```cpp
enum class Type {
    Null, Bool, Int, Float, String, Binary, Fn, Vector, Table, Struct, UserData
};
```

### BblValue

Tagged union holding any BBL value.  Access the appropriate member based on
`type`:

| Type         | Member          | C++ Type         |
|--------------|-----------------|------------------|
| `Null`       | --              | --               |
| `Bool`       | `boolVal`       | `bool`           |
| `Int`        | `intVal`        | `int64_t`        |
| `Float`      | `floatVal`      | `double`         |
| `String`     | `stringVal`     | `BblString*`     |
| `Binary`     | `binaryVal`     | `BblBinary*`     |
| `Fn`         | `fnVal`         | `BblFn*`         |
| `Fn` (C)     | `cfnVal`        | `BblCFunction`   |
| `Vector`     | `vectorVal`     | `BblVec*`        |
| `Table`      | `tableVal`      | `BblTable*`      |
| `Struct`     | `structVal`     | `BblStruct*`     |
| `UserData`   | `userdataVal`   | `BblUserData*`   |

Check `isCFn` to distinguish BBL functions from C functions when `type == Fn`.

### Factory Methods

```cpp
BblValue::makeNull()
BblValue::makeInt(int64_t)
BblValue::makeFloat(double)
BblValue::makeBool(bool)
BblValue::makeString(BblString*)
BblValue::makeBinary(BblBinary*)
BblValue::makeFn(BblFn*)
BblValue::makeCFn(BblCFunction)
BblValue::makeStruct(BblStruct*)
BblValue::makeVector(BblVec*)
BblValue::makeTable(BblTable*)
BblValue::makeUserData(BblUserData*)
```

---

## Struct Registration

Structs are POD value types with C++-identical binary layout.  Fields must be
numeric types, `bool`, or other registered structs.

### StructBuilder

```cpp
struct vertex { float x, y, z; };

BBL::StructBuilder builder("vertex", sizeof(vertex));
builder.field<float>("x", offsetof(vertex, x));
builder.field<float>("y", offsetof(vertex, y));
builder.field<float>("z", offsetof(vertex, z));
bbl.registerStruct(builder);
```

After registration, `vertex` becomes a callable constructor in script:

```bbl
(= v (vertex 1.0 2.0 3.0))
(print v.x)
```

### Supported Field Types

`field<T>()` supports: `float`, `double`, `int32_t`, `int64_t`, `bool`.

### Nested Structs

```cpp
struct triangle { vertex a, b, c; };

BBL::StructBuilder tb("triangle", sizeof(triangle));
tb.structField("a", offsetof(triangle, a), "vertex");
tb.structField("b", offsetof(triangle, b), "vertex");
tb.structField("c", offsetof(triangle, c), "vertex");
bbl.registerStruct(tb);
```

```bbl
(= tri (triangle (vertex 0 1 0) (vertex 1 0 0) (vertex -1 0 0)))
(print tri.a.x)    // chained dot reads work
```

### Validation

Registration validates:
- No overlapping fields
- No size overflow
- Field types are valid
- Re-registering the same layout is a silent no-op
- Re-registering a different layout is an error

### Properties

- `memcpy` copy semantics, no GC
- Type descriptors are global on `BblState`, never freed during execution
- No member functions -- structs are pure data

---

## Typed Vectors

Contiguous storage with the same binary layout as C++ `std::vector<T>`.  This
is the core value proposition for serialization and GPU interop.

### Zero-Copy Access

```cpp
bbl.exec(R"(
    (= verts (vector vertex
        (vertex 0 1 0)
        (vertex 1 0 0)
        (vertex -1 0 0)))
)");

vertex* data = bbl.getVectorData<vertex>("verts");
size_t count = bbl.getVectorLength<vertex>("verts");

// data is a raw pointer to contiguous vertex structs
// pass directly to OpenGL, Vulkan, serializer, etc.
```

### Allocating from C++

```cpp
BblVec* vec = bbl.allocVector("vertex", BBL::Type::Struct, sizeof(vertex));
// populate via script or packValue()
bbl.set("my-verts", BblValue::makeVector(vec));
```

---

## UserData Registration

Userdata wraps an opaque `void*` with a registered type descriptor providing
methods and an optional destructor.

### TypeBuilder

```cpp
struct FileHandle {
    FILE* fp;
    std::string path;
};

static int fileWrite(BblState* bbl) {
    auto* ud = bbl->getArg(0).userdataVal;  // self (first arg from colon call)
    auto* fh = static_cast<FileHandle*>(ud->data);
    if (!fh) throw BBL::Error{"write on closed file"};
    const char* str = bbl->getStringArg(1);
    fputs(str, fh->fp);
    return 0;
}

static int fileClose(BblState* bbl) {
    auto* ud = bbl->getArg(0).userdataVal;
    auto* fh = static_cast<FileHandle*>(ud->data);
    if (!fh) return 0;
    fclose(fh->fp);
    delete fh;
    ud->data = nullptr;  // prevent double-free by GC
    return 0;
}

static void fileDestructor(void* ptr) {
    auto* fh = static_cast<FileHandle*>(ptr);
    if (fh) {
        fclose(fh->fp);
        delete fh;
    }
}

BBL::TypeBuilder tb("File");
tb.method("write", fileWrite);
tb.method("close", fileClose);
tb.destructor(fileDestructor);
bbl.registerType(tb);
```

### Creating Userdata Instances

From a C function:

```cpp
static int myFopen(BblState* bbl) {
    const char* path = bbl->getStringArg(0);
    auto* fh = new FileHandle{fopen(path, "w"), path};
    bbl->pushUserData("File", fh);
    return 1;
}
```

From C++ directly:

```cpp
auto* ud = bbl.allocUserData("File", new FileHandle{fp, path});
bbl.set("output", BblValue::makeUserData(ud));
```

### Destructor Contract

- Called by GC during sweep, or by `with` block exit, or at `~BblState()`
- Must not call back into `BblState` or throw exceptions
- Must check for null data pointer (may have been closed explicitly)
- After `with` block, data pointer is set to null to prevent double-free

### Method Dispatch

From script, userdata methods are called via colon syntax:

```bbl
(f:write "hello")    // dispatches to fileWrite with f as first arg
(f:close)            // dispatches to fileClose
```

Resolution: type tag -> UserDataDesc -> method table -> call.

---

## Allocators

`BblState` provides allocators for all GC-managed types.  Allocated objects are
tracked by the GC and freed during sweep if unreachable.

| Method                                              | Returns         |
|-----------------------------------------------------|-----------------|
| `bbl.intern(const std::string& s)`                  | `BblString*`    |
| `bbl.allocBinary(std::vector<uint8_t> data)`         | `BblBinary*`    |
| `bbl.allocFn()`                                     | `BblFn*`        |
| `bbl.allocStruct(StructDesc* desc)`                 | `BblStruct*`    |
| `bbl.allocVector(elemType, elemTypeTag, elemSize)`   | `BblVec*`       |
| `bbl.allocTable()`                                  | `BblTable*`     |
| `bbl.allocUserData(typeName, void* data)`            | `BblUserData*`  |

### Table Operations

```cpp
BblTable* t = bbl.allocTable();
t->set(BblValue::makeString(bbl.intern("name")), BblValue::makeString(bbl.intern("hero")));
t->set(BblValue::makeInt(0), BblValue::makeString(bbl.intern("item")));

BblValue val = t->get(BblValue::makeString(bbl.intern("name")));
bool exists = t->has(BblValue::makeString(bbl.intern("hp")));
size_t len = t->length();
BblValue positional = t->at(0);    // by insertion order

bbl.set("player", BblValue::makeTable(t));
```

### Binary Operations

```cpp
std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};
BblBinary* bin = bbl.allocBinary(std::move(data));
bbl.set("header", BblValue::makeBinary(bin));

// reading back
BblBinary* b = bbl.getBinary("header");
size_t len = b->length();
uint8_t* ptr = b->data.data();
```

---

## Garbage Collection

Mark-and-sweep.  Managed objects: strings, binaries, closures, vectors, tables,
userdata.

### Configuration

```cpp
bbl.gcThreshold = 256;    // trigger GC after this many allocations (default)
```

Adaptive: after each sweep, threshold is set to `max(256, liveCount * 2)`.

### Manual Collection

```cpp
bbl.gc();
```

### GC Roots

- Scope chain (all live bindings from root through call stack)
- String intern table
- Type descriptor table

### Safe Points

GC triggers only at safe points (top of exec/execExpr, loop iterations) where
all live values are rooted in scope bindings.

---

## Standard Library

### Registration

```cpp
BBL::addStdLib(bbl);    // registers all modules below
// or pick individually:
BBL::addPrint(bbl);     // print function
BBL::addMath(bbl);      // math functions + constants
BBL::addFileIo(bbl);    // fopen, filebytes, File type, stdin/stdout/stderr globals
```

### Print Capture

Redirect `print` output to a C++ string for testing:

```cpp
std::string output;
bbl.printCapture = &output;
bbl.exec("(print \"hello\")");
// output == "hello"
bbl.printCapture = nullptr;  // restore stdout
```

---

## Security

### Filesystem Sandbox

```cpp
bbl.allowOpenFilesystem = false;    // default: scripts sandboxed
bbl.allowOpenFilesystem = true;     // scripts can use absolute paths, ..
```

When `false` (default), `execfile` and `filebytes` called from script reject:
- Absolute paths
- Parent directory traversal (`..`)

Paths resolve relative to the calling script's directory.  Each exec'd script's
sandbox root narrows to its own directory.

The `bbl` CLI binary sets `allowOpenFilesystem = true` for developer convenience.

`fopen` is not sandboxed (follows standard C semantics).

C++ API calls (`bbl.execfile()`) use the same sandbox flag.

---

## Error Handling

### Catching Errors in C++

```cpp
try {
    bbl.exec("(/ 1 0)");
} catch (const BBL::Error& e) {
    bbl.printBacktrace(e.what);
    // e.what contains the error message
}
```

### BBL::Error

```cpp
struct Error {
    std::string what;
};
```

Single error type for all runtime errors.  C functions can throw `BBL::Error`
directly; the backtrace will include the call site.

### Backtraces

```cpp
bbl.printBacktrace("division by zero");
```

Prints each frame (innermost first) with file, line, and abbreviated expression
to stderr.

---

## Data Structure Reference

### BblString

```cpp
struct BblString {
    std::string data;
    bool marked;        // GC mark flag
};
```

### BblBinary

```cpp
struct BblBinary {
    std::vector<uint8_t> data;
    bool marked;
    size_t length() const;
};
```

### BblVec

```cpp
struct BblVec {
    std::string elemType;       // e.g. "vertex", "int"
    BBL::Type elemTypeTag;
    size_t elemSize;
    std::vector<uint8_t> data;  // packed element storage
    bool marked;
    size_t length() const;
    uint8_t* at(size_t i);
    const uint8_t* at(size_t i) const;
};
```

### BblTable

```cpp
struct BblTable {
    std::vector<std::pair<BblValue, BblValue>> entries;
    int64_t nextIntKey;     // auto-increment for push (0-based)
    bool marked;
    size_t length() const;
    BblValue get(const BblValue& key) const;
    void set(const BblValue& key, const BblValue& val);
    bool has(const BblValue& key) const;
    bool del(const BblValue& key);
};
```

Table operations are O(n) linear scans.  Fine for small tables (< 50 entries).
For large collections, use vectors with integer indexing.

### BblFn

```cpp
struct BblFn {
    std::vector<std::string> params;
    std::vector<uint32_t> paramIds;
    std::vector<AstNode> body;
    std::vector<std::pair<uint32_t, BblValue>> captures;
    std::unordered_map<uint32_t, size_t> slotIndex;
    size_t paramSlotStart;
    bool marked;
};
```

### BblStruct

```cpp
struct BblStruct {
    StructDesc* desc;
    std::vector<uint8_t> data;  // raw bytes, C++-compatible layout
    bool marked;
};
```

### BblUserData

```cpp
struct BblUserData {
    UserDataDesc* desc;
    void* data;                 // opaque pointer
    bool marked;
};
```

### StructDesc

```cpp
struct FieldDesc {
    std::string name;
    size_t offset;
    size_t size;
    CType ctype;                // Int32, Int64, Float32, Float64, Bool, Struct
    std::string structType;     // for nested struct fields
};

struct StructDesc {
    std::string name;
    size_t totalSize;
    std::vector<FieldDesc> fields;
};
```

### UserDataDesc

```cpp
struct UserDataDesc {
    std::string name;
    std::unordered_map<std::string, BblCFunction> methods;
    BblUserDataDestructor destructor;   // void (*)(void*)
};
```

---

## Build Integration

### CMake

```cmake
add_executable(my-app main.cpp bbl.cpp bbl.h)
```

BBL is a single header + source pair.  No external dependencies beyond the C++
standard library.

### Building BBL Itself

```sh
cmake -B build && cmake --build build
./build/bbl_tests        # run tests
./build/bbl script.bbl   # run interpreter
```
