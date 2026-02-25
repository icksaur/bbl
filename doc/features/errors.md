# Errors

## goal

Simple, predictable error propagation.  When something goes wrong in BBL, the runtime prints a diagnostic with a backtrace and throws a C++ exception.  The C++ caller catches it.  No in-script error handling for v1.

## error type

```cpp
namespace BBL {
    struct Error {
        std::string what;    // human-readable message
        // backtrace is printed to stderr before throwing
    };
}
```

One type for all errors.  `what` contains the error message.  The backtrace is printed to stderr at the point of the error — it is not stored on the exception object (keeps the struct trivial).

## error propagation model

1. Runtime detects an error condition (type mismatch, out-of-bounds, file I/O failure, etc.).
2. Runtime prints to stderr:
   - Error message (e.g. `error: index 5 out of bounds (length 3)`)
   - Backtrace: the call stack from innermost to outermost, with file name and line number for each frame.
3. Runtime throws `BBL::Error{what}`.
4. C++ destructors run normally during stack unwinding.
5. The C++ caller catches the exception.

```cpp
BblState bbl;
BBL::addPrint(bbl);
try {
    bbl.execfile("script.bbl");
} catch (const BBL::Error& e) {
    fprintf(stderr, "bbl failed: %s\n", e.what.c_str());
}
// ~BblState cleans up regardless — GC frees all managed objects
```

## backtrace format

```
error: type mismatch: expected vertex, got int
  at (verts.push 42)        script.bbl:14
  at (load-mesh)            script.bbl:22
  at (execfile "script.bbl")    main.bbl:3
```

Each line shows:
- The expression that failed (abbreviated to the form)
- The source file and line number

Frames are listed innermost-first.  `execfile` and `exec` boundaries show as frames in the trace since they represent real call boundaries.

## error conditions

These are the runtime conditions that produce errors:

| category | example | message |
|----------|---------|---------|
| type mismatch | `(+ 1 "hello")` | `type mismatch: + cannot apply to int and string` |
| non-bool condition | `(if 42 (print "yes"))` | `type mismatch: condition must be bool, got int` |
| out of bounds | `(verts.at 99)` on a 3-element vector | `index 99 out of bounds (length 3)` |
| undefined symbol | `(print x)` where `x` was never defined | `undefined symbol: x` |
| undefined on set | `(set y 5)` where `y` was never defined | `undefined symbol: y` |
| arity mismatch | `(greet)` where `greet` takes 1 arg | `arity mismatch: greet expects 1 argument, got 0` |
| struct constructor | `(vertex 1.0 2.0)` (missing z) | `vertex constructor expects 3 fields, got 2` |
| vector type | `(verts.push "hello")` into a `vector vertex` | `type mismatch: expected vertex, got string` |
| division by zero | `(/ x 0)` | `division by zero` |
| file I/O | `(filebytes "missing.bin")` | `file read failed: missing.bin` |
| parse error | `(def x (+ 1)` (missing `)`) | `parse error: expected ')' at script.bbl:1` |
| pop empty | `(verts.pop)` on empty vector | `pop on empty vector` |
| no field | `(print v.w)` on a vertex with x/y/z | `struct "vertex" has no field "w"` |
| no field | `(v.push 1)` on a struct | `struct "vertex" has no field "push"` |
| dot on wrong type | `(def x 5)` then `x.foo` | `type error: int has no fields or methods` |
| chained write | `(set tri.a.x 5.0)` | `error: only single-level place writes allowed` |

## what errors do NOT do

- **No in-script catch/try.**  Errors always propagate to the C++ caller.  Script-level error handling is deferred (see backlog).
- **No error codes.**  One exception type, one path.
- **No recovery.**  After an error, the script is done.  The `BblState` is still valid for cleanup (`~BblState` runs normally), but you cannot resume execution.
- **No warnings.**  If it's wrong, it's an error.  If it's fine, it's silent.

## C function errors

C functions registered via `bbl.defn()` can signal errors by throwing `BBL::Error` directly:

```cpp
int my_fopen(BblState* bbl) {
    const char* path = bbl->getStringArg(0);
    FILE* f = fopen(path, "r");
    if (!f) {
        throw BBL::Error{"fopen failed: " + std::string(path)};
    }
    // ...
    return 1;
}
```

The backtrace will include the C function's call site in BBL script.

## implementation notes

The backtrace requires the interpreter to maintain a call stack of `{file, line, expression}` frames.  This is a `std::vector<Frame>` on `BblState`, pushed on function/execfile/exec entry and popped on return.  The cost is one push/pop per call — negligible.

Parse errors throw immediately during parsing (before execution begins).  The backtrace for parse errors is just the file and line — there's no runtime call stack yet.

## open questions

None.
