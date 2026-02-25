# errors

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
4. All C++ destructors (RAII) run normally during stack unwinding — refcounts decrement, file handles close, memory frees.
5. The C++ caller catches the exception.

```cpp
BblState bbl;
BBL::addPrint(bbl);
try {
    bbl.exec("script.bbl");
} catch (const BBL::Error& e) {
    fprintf(stderr, "bbl failed: %s\n", e.what.c_str());
}
// ~BblState cleans up regardless
```

## backtrace format

```
error: type mismatch: expected vertex, got int32
  at [verts.push 42]        script.bbl:14
  at [load-mesh]            script.bbl:22
  at [exec "script.bbl"]    main.bbl:3
```

Each line shows:
- The expression that failed (abbreviated to the bracket form)
- The source file and line number

Frames are listed innermost-first.  `exec` boundaries show as frames in the trace since they represent real call boundaries.

## error conditions

These are the runtime conditions that produce errors:

| category | example | message |
|----------|---------|---------|
| type mismatch | `[+ 1 "hello"]` | `type mismatch: + cannot apply to int32 and string` |
| out of bounds | `[verts.at 99]` on a 3-element vector | `index 99 out of bounds (length 3)` |
| undefined symbol | `[print x]` where `x` was never defined | `undefined symbol: x` |
| arity mismatch | `[greet]` where `greet` takes 1 arg | `arity mismatch: greet expects 1 argument, got 0` |
| struct constructor | `[vertex 1.0 2.0]` (missing z) | `vertex constructor expects 3 fields, got 2` |
| vector type | `[verts.push "hello"]` into a `vector vertex` | `type mismatch: expected vertex, got string` |
| division by zero | `[/ x 0]` | `division by zero` |
| file I/O | lazy-load binary from truncated file | `binary load failed: unexpected EOF at offset 4096 in scene.bbl` |
| parse error | `[= x [+ 1]` (missing `]`) | `parse error: expected ']' at script.bbl:1` |
| pop empty | `[verts.pop]` on empty vector | `pop on empty vector` |
| self-reference | `[m.set "self" m]` | `cycle detected: map cannot contain itself` |\n| struct redefinition | `[= vertex [struct [f32 x]]]` when `vertex` already exists with different layout | `struct redefinition: vertex has a different layout than the existing registration` |\n| non-bool condition | `[cond [[42 ...]]]` | `type mismatch: condition must be bool, got int32` |

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

The backtrace requires the interpreter to maintain a call stack of `{file, line, expression}` frames.  This is a `std::vector<Frame>` on `BblState`, pushed on function/exec entry and popped on return.  The cost is one push/pop per call — negligible.

Parse errors throw immediately during parsing (before execution begins).  The backtrace for parse errors is just the file and line — there's no runtime call stack yet.
