# Deterministic Userdata Destructor Timing
Status: implemented

## Goal

Provide a language-level guarantee that a userdata destructor runs at a predictable time — when the `with` block exits — rather than relying on GC timing.  This enables RAII patterns (file handles, locks, GPU resources) where cleanup must happen promptly, not "eventually."

`with` is a language primitive.  It works the same way in every execution context — library, binary interpreter, embedded host.  There is no flag or per-type configuration.  The same script always produces the same behavior.

## Background

### Current behavior

Userdata objects are GC-managed.  The destructor registered via `TypeBuilder::destructor()` runs in two places:

1. **GC sweep** (`BblState::gc()`, ~line 697 in bbl.cpp) — when the object is unmarked (unreachable).
2. **`~BblState()`** (~line 508 in bbl.cpp) — when the interpreter shuts down.

Neither guarantees prompt cleanup.  The GC runs when `allocCount >= gcThreshold` (adaptive, starting at 256).  A script that opens a file, stops referencing it, and does no further allocations may never trigger GC — the file handle leaks until `~BblState`.

### Current workaround

The `File` type exposes an explicit `(f.close)` method.  Scripts must call it manually.  The destructor (`fileDestructor`) is a safety net, not a guarantee.

### Spec non-goal update

`doc/spec.md` lists "deterministic destruction" as a non-goal.  This was written before `with` existed.  With `with`, the spec should be updated: remove "deterministic destruction" from non-goals.  `with` makes deterministic cleanup a core language feature, not an afterthought.  The GC still handles objects not wrapped in `with` — those are freed eventually, as before.

### Scope mechanics

BBL scopes (`BblScope`) operate in two modes: **hash-map mode** (using `def()` to insert bindings into an `unordered_map`) and **flat-slot mode** (pre-allocated vector indexed by slot map, used by `fn` calls for performance).  `with` uses hash-map mode via `def()` — it only binds one symbol, so the overhead is negligible.

`BblScope::parent` chains scopes for variable lookup.  A `with` scope has `parent = &enclosingScope`, so body code can read/write enclosing bindings.  `resolveSymbol()` converts a string name to a `uint32_t` ID used as the hash-map key.

Active scopes are tracked in `BblState::activeScopes` (a `vector<BblScope*>`).  The GC marks all active scopes as roots.  `with` pushes its scope on entry and pops on exit, matching the `callFn` pattern.

BBL currently has no `break`/`continue` (see backlog).  All control flow is via normal returns or `BBL::Error` exceptions.  If `break`/`continue` are added later (likely as sentinel exceptions), the try/catch in `with` will correctly intercept them, run the destructor, and re-throw — no special handling needed.

## Use Cases

### Basic resource cleanup

```bbl
(with f (fopen "data.bin" "rb")
    (= data (f.read))
    (print data))
// f's destructor (fclose) runs here
```

### Nested resources (LIFO order)

```bbl
(with src (fopen "input.txt" "r")
    (with dst (fopen "output.txt" "w")
        (dst.write (src.read))))
// dst closed first, then src — matches C++ destructor order
```

### Escape hazard (footgun)

```bbl
(= leaked null)
(with f (fopen "data.txt" "r")
    (= leaked f))
// f's destructor runs here — leaked now holds a dead reference
(leaked.read)   // throws: "File.read: file is closed"
```

The script author sees the error from the type's method (not from `with`).  Each type's methods must check `data != nullptr` and throw a clear error.

### No-destructor type

```bbl
(with obj (make-widget)
    (obj.update)
    (obj.render))
// no destructor registered for Widget — with exits without cleanup call
// obj is eventually freed by GC (only the BblUserData wrapper, not the void* data)
```

## C++ API for with-compatible types

A library user registers their own deterministically-destructed types using the same `TypeBuilder` + `allocUserData` pattern that the standard library's `File` type uses.  No extra API is needed — any type with a destructor works with `with` automatically.

### Registering a type

```cpp
// 1. Write a destructor — a void(*)(void*) that cleans up the raw resource.
static void gpuBufferDestructor(void* ptr) {
    GpuBuffer* buf = static_cast<GpuBuffer*>(ptr);
    buf->release();           // e.g. glDeleteBuffers
    delete buf;
}

// 2. Write methods — BblCFunction (int(*)(BblState*)).
static int gpuBufferUpload(BblState* bbl) {
    BblValue self = bbl->getArg(0);                     // first arg is receiver
    GpuBuffer* buf = static_cast<GpuBuffer*>(self.userdataVal->data);
    if (!buf) throw BBL::Error{"GpuBuffer.upload: buffer is released"};
    BblBinary* b = bbl->getBinaryArg(1);
    buf->upload(b->data.data(), b->length());
    return 0;
}

// 3. Register with TypeBuilder.
void addGpuTypes(BblState& bbl) {
    BBL::TypeBuilder tb("GpuBuffer");
    tb.method("upload", gpuBufferUpload)
      .method("bind",   gpuBufferBind)
      .method("release", gpuBufferRelease)   // optional manual close
      .destructor(gpuBufferDestructor);
    bbl.registerType(tb);
    bbl.defn("gpu-buffer", gpuBufferCreate);  // factory cfn
}
```

### Factory function

The factory cfn allocates the resource and returns a userdata value:

```cpp
static int gpuBufferCreate(BblState* bbl) {
    int64_t size = bbl->getIntArg(0);
    GpuBuffer* buf = new GpuBuffer(static_cast<size_t>(size));
    bbl->pushUserData("GpuBuffer", buf);     // wraps in BblUserData
    return 0;
}
```

### Script usage with `with`

```bbl
(with buf (gpu-buffer 4096)
    (buf.upload payload)
    (buf.bind))
// buf.release() + delete called here by destructor
```

### Null-data contract

When `with` runs the destructor, it sets `data = nullptr` on the `BblUserData`.  If the type also has an explicit "release" or "close" method, that method **must** null the data pointer to prevent double-free.  The `File` type demonstrates this:

```cpp
static int bblFileClose(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (fp) {
        fclose(fp);
        self.userdataVal->data = nullptr;   // prevents double-free
    }
    return 0;
}
```

Every method should check for null data and throw a descriptive error:

```cpp
if (!buf) throw BBL::Error{"GpuBuffer.upload: buffer is released"};
```

This is a convention, not enforced by the runtime.  Violating it risks a crash on use-after-destroy.

## Design

### Approach: block `with` (primary)

Introduce a `with` special form that takes a userdata binding and a scope body.  The destructor runs when the body completes (or throws), regardless of GC timing.

```bbl
(with f (fopen "data.bin" "rb")
    (= data (f.read))
    (print data))
// f's destructor runs here, even if (f.read) throws
```

`with` evaluates the initializer, binds the result to the given symbol in a new scope, evaluates the body, then calls the type's destructor before returning.  The body's last expression is the return value.

If no destructor is registered for the type, `with` still works — it just doesn't call a destructor.  This makes `with` safe with any userdata.

### Semantics

1. `(with name init body...)` — new special form.
2. `init` is evaluated in the enclosing scope.  Must produce a userdata value; otherwise, runtime error.
3. `name` is bound in a new child scope (like `fn` call scope).
4. `body` expressions are evaluated sequentially in that scope.
5. On scope exit (normal return or exception), the destructor is called on the userdata's `data` pointer, and the `data` pointer is set to `nullptr` (preventing double-free by GC).
6. The return value is the last expression in `body`.
7. If `body` throws, the destructor still runs, then the exception is re-thrown.

### Implementation approach

Files: `bbl.cpp` (SpecialForm enum, lookup table, evalList switch), `tests/test_bbl.cpp`, `doc/bbl.md`.

1. Add `With` to the `SpecialForm` enum.
2. Add `{"with", SpecialForm::With}` to the lookup table.
3. Add evaluation logic in the `switch(sf)` block:

```cpp
case SpecialForm::With: {
    // (with name init body...)
    if (node.children.size() < 3)
        throw BBL::Error{"with requires a name, initializer, and body"};
    if (node.children[1].type != NodeType::Symbol)
        throw BBL::Error{"with: first argument must be a symbol"};
    const std::string& name = node.children[1].stringVal;
    BblValue val = eval(node.children[2], scope);
    if (val.type != BBL::Type::UserData)
        throw BBL::Error{"with: initializer must produce userdata, got " + typeName(val.type)};
    BblUserData* ud = val.userdataVal;
    BblScope withScope;
    withScope.parent = &scope;
    uint32_t nameId = resolveSymbol(name);
    withScope.def(nameId, val);
    activeScopes.push_back(&withScope);

    auto cleanup = [&]() {
        activeScopes.pop_back();
        if (ud->desc && ud->desc->destructor && ud->data) {
            ud->desc->destructor(ud->data);
            ud->data = nullptr;
        }
    };

    BblValue result = BblValue::makeNull();
    try {
        for (size_t i = 3; i < node.children.size(); i++) {
            result = eval(node.children[i], withScope);
        }
    } catch (...) {
        try { cleanup(); } catch (...) { /* swallow destructor exception */ }
        throw;
    }
    cleanup();
    return result;
}
```

The `cleanup` lambda is called in both normal and exceptional paths.  In the exception path, the destructor call is wrapped in an inner try/catch that swallows any destructor exception — the original exception takes priority.  This matches C++ RAII semantics: cleanup errors are silently discarded rather than replacing the original error.

4. Setting `ud->data = nullptr` after destruction prevents the GC and `~BblState` from calling the destructor again.  The existing GC sweep and destructor code already check `u->data` for null.

### Double-free prevention

After `with` runs the destructor, it sets `data = nullptr`.  Both the GC sweep and `~BblState` check `u->data` before calling the destructor:

```cpp
if ((*it)->desc && (*it)->desc->destructor && (*it)->data) {
    (*it)->desc->destructor((*it)->data);
}
```

This null-pointer guard already exists in the codebase.  No additional changes needed for double-free safety.

### Escape analysis

If the script stores the userdata in a table or other container that outlives the `with` scope, the object's `data` will be null after `with` exits.  Methods called on the escaped reference will see `data == nullptr`.  This is the same situation as calling `(f.close)` and then using the file — the method should check for null and throw.

The `File` type already handles this: `if (!fp) throw BBL::Error{"File.read: file is closed"};`.  This pattern is the responsibility of each type's method implementations.

### Why not per-type automatic scope tracking?

An alternative design would tag a type as "scope-bound" at registration time, and automatically run destructors when any binding goes out of scope.  This was considered and rejected:

- **Shared references break it.**  If a userdata is bound in multiple scopes (passed as argument, stored in table), which scope "owns" it?  The first to exit would destroy it, leaving dangling references.
- **Closures capture userdata.**  A closure capturing a scope-bound userdata would either need reference counting or would dangle after the scope exits.
- **Complexity.**  Automatic tracking requires ownership semantics that conflict with BBL's shared-reference GC model.

`with` avoids these problems by making the cleanup point explicit.  The script author chooses where cleanup happens, and the language guarantees it runs.

### Why not `with` + closeable interface?

Another alternative: a `with` block that calls a `.close` method instead of the destructor.  This was considered and rejected:

- **Redundant.**  Types already have destructors.  Adding a separate "closeable" interface doubles the cleanup machinery.
- **Method vs destructor mismatch.**  The destructor is a C function pointer on the type descriptor.  A `.close` method is script-callable and may have side effects the destructor doesn't.  Keeping cleanup in the destructor is simpler and consistent with C++ RAII.

### Flat `defer` — feasibility analysis

The block form `(with name init body...)` requires nesting when managing multiple resources:

```bbl
(with f (fopen "a" "r")
    (with g (fopen "b" "w")
        (g.write (f.read))))
```

A flat form would allow sequential defers without nesting:

```bbl
(= f (fopen "a" "r"))
(defer f)
(= g (fopen "b" "w"))
(defer g)
(g.write (f.read))
// g destroyed, then f destroyed (LIFO)
```

This is closer to Go's `defer`, which attaches cleanup to the enclosing **function** rather than a block.

#### The scope problem

BBL does not have reliable block-level scopes.  `do`, `if`, `loop`, and `each` all evaluate in the enclosing `scope&` — they don't create or destroy their own `BblScope`.  The only construct that creates and tears down a scope is `callFn` (function calls).  This means "the enclosing scope" is not a well-defined cleanup point for arbitrary code:

| Context | Scope passed to eval | Has exit point? |
|---------|---------------------|----------------|
| Top-level (`exec`) | `rootScope` | No — lives until `~BblState` |
| Inside `fn` body (`callFn`) | `callScope` | **Yes** — `callFn` return |
| Inside `do` | inherited from caller | No — pass-through |
| Inside `if`/`loop`/`each` | inherited from caller | No — pass-through |

So `(defer f)` inside a `do` block would not clean up at the `do` exit — it would clean up at the nearest function exit (or never, if at top level).

#### Go-style function-exit approach

The cleanest flat design: `(defer symbol)` registers cleanup that runs when the current **call frame** exits.  Implementation:

1. Add a `std::vector<BblUserData*> deferList` field to `BblScope` (or, more precisely, a parallel stack in `BblState` that shadows the call stack).
2. `(defer x)` looks up `x` in the current scope.  If it's userdata, push its `BblUserData*` onto the current frame's defer list.
3. In `callFn`, after the body try/catch, iterate the defer list in reverse (LIFO) and run each destructor + null the data pointer.
4. For top-level code, `exec()` would need a matching defer list that runs when `exec()` returns.

```cpp
// In callFn, after body evaluation:
for (auto it = callScope.deferList.rbegin(); it != callScope.deferList.rend(); ++it) {
    BblUserData* ud = *it;
    if (ud->desc && ud->desc->destructor && ud->data) {
        try { ud->desc->destructor(ud->data); } catch (...) { /* swallow */ }
        ud->data = nullptr;
    }
}
```

**Complexity: moderate.**  ~20 lines in `callFn`, ~15 lines for the `Defer` one-arg case, ~15 lines for `exec()` wrapper.  The `BblScope` struct gains one `std::vector<BblUserData*>` (cheap when empty).  Total: ~50-60 lines.

#### Tradeoffs vs block form

| | Block `(with name init body...)` | Flat `(defer name)` |
|---|---|---|
| Multiple resources | Nested indentation | Sequential, no nesting |
| Lifetime visibility | Explicit — delimited by body | Implicit — ends at function return |
| Use-after-destroy risk | Low — name not in scope after block | Higher — name still in scope after defer |
| Scope dependency | None — self-contained | Requires function call frame |
| Top-level behavior | Works (creates own scope) | Needs `exec()` to manage a defer list |
| Implementation | ~30 lines | ~50-60 lines, touches more code |
| GC interaction | Same | Same |

#### The `do` question

The user's example expects cleanup at `do` exit:

```bbl
(do
    (= f (fopen "a" "r"))
    (defer f)
    ...
) // f destroyed here?
```

With function-exit semantics, `f` would **not** be destroyed here — it would be destroyed when the enclosing `fn` returns.  To make `do` a cleanup boundary, `do` would need to:

1. Create its own `BblScope`.
2. Push/pop `activeScopes`.
3. Run any defers on exit.

This is a **larger change**: `do` currently just loops over children in the parent scope.  Making it scope-creating changes semantics — bindings introduced by `=` inside `do` would no longer be visible after the `do` block.  That could be desirable (lexical scoping) but is a **breaking change** to current behavior.

Alternatively, a new form like `(scope ...)` could be introduced that explicitly creates a scope boundary with defer semantics, leaving `do` unchanged.  But this adds another form that users need to learn.

#### Recommendation

Implement the **block form (`with`) first** — it's simpler, self-contained, and doesn't depend on scope semantics.  The flat form can be added later as `defer` (a separate special form with Go-style function-exit semantics).  The two forms are complementary:

- `with` for short, focused resource usage (block-scoped, Lisp-idiomatic).
- `defer` for functions that manage multiple resources over their whole lifetime (future work).

If flat `defer` is added, it should use function-exit semantics (Go model), not block-exit, because BBL's `do`/`if`/`loop` don't create scope boundaries.  Top-level flat `defer` would run at `exec()` return.

## Considerations

- **`with` is a scope.**  Like `fn` call, it creates a new scope using hash-map mode (`BblScope::def()`).  The binding is only visible inside the `with` body.  This is intentional — it prevents accidental use after destruction.
- **Nesting.**  Multiple `with` blocks can nest.  Inner `with` blocks run first (LIFO), matching C++ destructor order.
- **Exception safety.**  The destructor runs in both normal and exceptional exits.  If the destructor itself throws during the exception cleanup path, the destructor's exception is silently swallowed — the original body exception takes priority.  On the normal path, a throwing destructor propagates normally.
- **GC interaction.**  The GC may run during the `with` body.  Since the binding is in scope (and the scope is in `activeScopes`), the userdata is reachable and won't be collected.  After `with` exits and sets `data = nullptr`, the GC will eventually sweep the `BblUserData` wrapper object itself (but won't call the destructor again).
- **Non-userdata types.**  Passing a non-userdata value to `with` is a runtime error.  `with` is specifically for resource cleanup.
- **spec.md update.**  Remove "deterministic destruction" from the non-goals list.  `with` makes it a language feature.
- **Empty body.**  `(with f (fopen "x" "r"))` with no body is valid — the destructor runs immediately after initialization.  This is a degenerate case but not harmful.
- **Return value.**  `with` returns the last expression in the body, making it composable: `(= data (with f (fopen "x" "rb") (f.read)))`.
- **`do` vs `with`.**  `do` runs in the enclosing scope and has no cleanup semantics.  `with` creates a new scope with guaranteed cleanup.  They are complementary.
- **`break`/`continue`.**  BBL does not currently have `break`/`continue` (see backlog).  If added later as sentinel exceptions, the try/catch in `with` will correctly intercept them, run the destructor, and re-throw.  No special handling needed.

### Error messages

| Condition | Error message |
|-----------|---------------|
| Missing arguments | `"with requires a name, initializer, and body"` |
| First arg not a symbol | `"with: first argument must be a symbol"` |
| Initializer not userdata | `"with: initializer must produce userdata, got <type>"` |
| Method on escaped/destroyed object | Type-specific (e.g. `"File.read: file is closed"`) |

## Risks

- **Escape hazard.**  If userdata escapes the `with` block (stored in a table, captured by a closure), methods called after `with` exits will see `data == nullptr`.  Each type's methods must check for null and throw a descriptive error (e.g. `"File.read: file is closed"`).  This is already the convention (File type does it), but unaudited types could crash.  Mitigation: document this contract clearly in the type registration docs.
- **Destructor re-entry.**  If the destructor is called explicitly via a method (e.g. `(f.close)`) inside the `with` body, and the method doesn't null the pointer, `with` will call the destructor again.  Mitigation: types that expose manual close should null `data` in their close method (File already does this).
- **Destructor throws during cleanup.**  If the body throws and the destructor also throws, the destructor's exception is silently swallowed.  This means destructor errors during unwind are invisible.  Mitigation: destructors should not throw — this is the same contract as C++ destructors.  The swallow behavior is preferable to the alternative (losing the original exception).

## Acceptance

1. `(with f (fopen "test.txt" "w") (f.write "hello"))` — file is closed after the body, not at GC time.
2. `(with f (fopen "test.txt" "w") (f.write "hello") (= x 42))` — returns `42` (last body expression).
3. Exception during body: destructor still runs.
4. After `with` exits, `f` is not accessible in the enclosing scope (scoped binding).
5. GC sweep does not double-call the destructor (data pointer is null).
6. `~BblState` does not double-call the destructor.
7. `(with f 42 (print f))` — runtime error: initializer must produce userdata.
8. `(with f)` — runtime error: requires initializer and body.
9. Non-destructored type: `with` still works, just no destructor call.
10. Nested `with` blocks: inner destructor runs before outer.
11. Explicit close inside `with`: no double-free (close method nulls data).
12. Existing userdata tests unchanged (GC-timed destruction still works for types not using `with`).
