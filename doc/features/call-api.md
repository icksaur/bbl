# bbl.call and Embedding Improvement Areas

Status: proposed

## Part 1: bbl.call — Invoke Pre-Compiled BBL Functions from C++

### Goal

Add `BblValue BblState::call(BblValue callable, args...)` so C++ can invoke
a pre-compiled BBL closure or C function without re-parsing source text.

Today the only way to call BBL code from C++ is `exec("(my-fn 42)")`, which
parses, compiles, and JIT-compiles from scratch every call.

### API

```cpp
BblValue BblState::call(BblValue callable, std::initializer_list<BblValue> args);
BblValue BblState::call(BblValue callable, std::span<const BblValue> args);
BblValue BblState::call(BblValue callable);  // no-arg convenience
```

Usage:
```cpp
auto fn = bbl.get("square").value();
BblValue result = bbl.call(fn, {BblValue::makeInt(7)});  // 49
```

### Implementation

Wraps `jitCall` (~10 lines). For closures: lazy-JIT if needed, copy callable
into `regs[base]` and args into `regs[base+1..N]`, invoke native code.
For C functions: populate `callArgs` and invoke directly.

Key details:
- **Stack safety**: Use `execDepth`-based offset into `vm->stack` so nested
  `call()` within an active `exec()` doesn't clobber live registers.
- **GC roots**: Args must be in the register file (GC-visible) before the
  call begins, since the callee may allocate and trigger GC.
- **Re-entrancy**: Check `execDepth < MAX_EXEC_DEPTH` (currently 64).
- **Error path**: Check global `g_jitError` after call, convert to `BBL::Error`.

### Acceptance Criteria

1. Works for closures, C functions, and BblFn
2. Performance: < 200ns per call (no parsing/compiling)
3. Errors propagate as `BBL::Error`
4. NOT thread-safe (same thread as BblState owner)
5. Re-entrancy bounded by MAX_EXEC_DEPTH
6. Unit tests for all callable types and edge cases

---

## Part 2: Language Pain Points for Real-World Embedding

Thought experiment: if BBL were used to implement programs similar to the
user's peer C++ projects (vkobjects, imgui, physics, http, synth), what
language-level improvements would make BBL more capable?

These are not proposals to rewrite peer projects in BBL, but to identify
areas where BBL's design creates friction for general-purpose use.

### Pain Point 1: No Mutable References

**Problem**: C/C++ APIs frequently use out-parameters (`float& v`, `bool& b`).
ImGui widgets modify values in-place: `slider(hp, "health")` mutates `hp`.
BBL has no mutable reference concept — all values are passed by copy.

**Current workaround**: Return the new value and reassign:
```bbl
(= hp (gui:slider hp "health"))
```
This works but is noisy. Every widget call requires `(= var (widget var ...))`.

**For tweens/animations**: Tables work as mutable containers:
```bbl
(= player (table "hp" 100 "x" 0.0 "y" 0.0))
(tween:to player "x" 10.0 0.5)  // mutates player.x over 0.5 seconds
```
Tables are reference types in BBL — passing a table to a function gives the
function a mutable handle. This naturally models mutable game objects.

**Note on GC roots**: C++ code holding `BblValue` references to tables
(e.g., a tween system holding a table across frames) must ensure the
value is reachable from BBL globals or the register file, otherwise GC
may collect it. Store the table in a BBL global or use `GcPauseGuard`.

**What BBL could add**: Nothing strictly required. The `(= var (f var))`
pattern is idiomatic in functional languages. Tables cover the mutable-object
case. The friction is real but manageable.

**Alternative**: A `set!` or `swap!` special form that modifies a variable
in the caller's scope from within a function would be powerful but dangerous
(violates lexical scoping). Not recommended.

### Pain Point 2: Enum/Constant Registration

**Problem**: GUI frameworks, Vulkan, and game engines have hundreds of enum
constants. ImGui alone has 500+. Registering each as a global integer is
tedious:
```cpp
bbl.setInt("ImGuiWindowFlags-NoTitleBar", ImGuiWindowFlags_NoTitleBar);
// ... repeat 500 times
```

**Solution**: BBL uses interned strings. Since `strcmp` on interned strings
is O(1) pointer comparison, string enums are zero-cost:
```bbl
(cmd:bind-graphics "shadow-pipeline")
(gui:begin-box "top-left" 10 10 220 160)
```
This is the Lua approach and it works. No language change needed — just
good API design in the C bindings.

**For bitwise flags**: Use integers with `bor`:
```bbl
(= flags (bor WindowFlags.NoTitleBar WindowFlags.NoResize))
```
A batch registration helper would reduce boilerplate:
```cpp
bbl.defnTable("WindowFlags", {{"NoTitleBar", 1}, {"NoResize", 2}, ...});
```
This creates a BBL table `WindowFlags` with integer fields. Trivial C++ helper,
no language change.

### Pain Point 3: No Atomic Buffer Swap

**Problem**: Audio synthesis requires a producer (BBL main thread fills sample
buffer) and consumer (audio thread reads buffer) with lock-free handoff.
BBL's child state messaging uses `std::mutex` + `std::condition_variable`,
which is too heavy for audio-thread communication.

**Current state**: Child states communicate via `MessageQueue` (mutex-guarded
deque). Values are serialized/copied between states. Vector payloads are
moved (zero-copy for the data, but the queue push/pop is mutex-locked).

**What BBL needs**: An atomic double-buffer primitive:
```bbl
(= audio-buf (atomic-buffer (vector float32)))
(audio-buf:resize 1024)

// Producer (main BBL thread):
(= write-buf (audio-buf:write-handle))
(each i (range 1024)
  (write-buf:set i (sin (* ...))))
(audio-buf:swap)  // atomic pointer swap, lock-free

// Consumer (C++ audio callback):
// float* samples = atomicBuf->readPtr();  // lock-free read
```

Implementation: two `BblVec*` pointers + `std::atomic<int>` index.
SPSC invariant: only the producer calls `swap()`, reader only calls `readPtr()`.

C++ side:
```cpp
struct AtomicBuffer {
    BblVec* vecs[2];
    std::atomic<int> readIdx{0};
    const float* readPtr() { return (const float*)vecs[readIdx.load(std::memory_order_acquire)]->data.data(); }
    // Called ONLY by producer thread:
    void swap() { readIdx.store(1 - readIdx.load(std::memory_order_relaxed), std::memory_order_release); }
};
```

GC safety: both `BblVec*` are on `gcHead` (allocated via `allocVector`).
The mark phase only writes `marked` flags, so concurrent audio reads are safe.

This is implementable as a userdata type with no language changes — just a
new stdlib module `(= buf (audio:double-buffer "float32" 1024))`.

### Pain Point 4: GC Pauses in Real-Time Loops

**Problem**: BBL's mark-sweep GC is stop-the-world. Any allocation can
trigger a collection cycle with unbounded pause time. For 60fps render
loops (16.6ms budget) or audio (23µs budget), this causes frame drops
and audio glitches.

**Current mitigation**: `GcPauseGuard` RAII helper pauses GC during
critical sections. `gcThreshold` can be tuned to defer collection.
For game loops (16ms budget), this is already sufficient: `pauseGC()`
before the frame, `resumeGC()` after, manual `gc()` during loading
screens. The real pain is audio (23µs budget) where even a single
GC check is risky.

**What BBL could add**:
- **Incremental GC**: Mark a few objects per frame instead of all at once.
  Bounded pause times (e.g., max 100µs per step). Major engineering effort.
- **Generational GC**: Young/old generation split. Most short-lived
  allocations collected cheaply. Moderate engineering effort.
- **GC-free execution mode**: Mark certain closures as "no-alloc" at
  compile time (only arithmetic, struct field access, vector indexing).
  These can be called with a guarantee of no GC trigger. Useful for audio.

Recommendation: generational GC is the best ROI. Most real-time loops
create short-lived temporaries that die young — generational collection
handles this with minimal pause overhead.

### Pain Point 5: No Multiple Return Values

**Problem**: C APIs frequently return multiple values — ImGui's
`SliderFloat` returns both `bool changed` and `float newValue`. Physics
contact queries return hit + point + normal. Currently BBL C functions
can only return one value, forcing table allocation:

```cpp
// Current: must allocate table for multi-return
bbl->pushTable(result);  // GC pressure per call
return 1;
```

**What BBL could add**: Multiple return values with destructuring:
```bbl
(= changed new-hp (gui:slider hp "health"))
```
C side:
```cpp
bbl->pushBool(changed);
bbl->pushFloat(newValue);
return 2;  // two return values
```

Implementation in two phases:
- **Phase 1 (small)**: C function multi-return via `returnValues` vector on
  BblState. C functions `return N`, caller unpacks from the vector. No JIT
  changes — only C-function dispatch path + compiler destructuring syntax.
- **Phase 2 (large)**: BBL closure multi-return. The JIT calling convention
  returns a single `BblValue` in `rax`. Multi-return requires either changing
  the convention (breaking all JIT'd functions) or a side-channel. Also
  affects inline caching and speculative callee inlining which assume single
  return via OP_RETURN.

### Pain Point 6: Call Overhead for High-Frequency Bindings

**Problem**: ImGui-style APIs make hundreds of small calls per frame.
Each BBL→C function call currently does:
1. Global hash table lookup for function name
2. `callArgs.clear()` + `push_back()` per argument
3. Type-checking per argument in the C function
4. Return value extraction

At 500 calls/frame × 60fps, this is ~30K calls/sec. Each call costs
~500ns (hash lookup + vector ops), totaling ~15ms/sec — under 1ms/frame.

**Assessment**: This is already fast enough. The JIT's inline caching for
known callees (speculative callee guard) eliminates the hash lookup for
hot calls. No action needed unless profiling shows otherwise.

### Pain Point 7: No Coroutines / Yield

**Problem**: HTTP servers need to handle concurrent requests. A BBL HTTP
handler that does `(= response (db:query ...))` blocks the entire BBL
thread. No other requests can be processed until the query returns.

**Current workaround**: Child states (one per connection). Each gets its
own thread and BblState. Works but heavy — each state has full GC heap,
intern table, symbol table.

**What BBL could add**: Cooperative coroutines with `yield`:
```bbl
(= handler (fn (ctx)
  (= data (yield (db:query "SELECT ...")))  // suspend, resume when ready
  (http:ok (json:encode data))))
```

This is a significant language feature (coroutine transform in compiler,
continuation capture in VM). Out of scope for near-term, but would unlock
efficient async I/O.

**Lighter alternative**: An event-loop primitive in C++ that multiplexes
multiple pending operations and drives BBL callbacks when ready:
```bbl
(= server (http:create 8080))
(http:get server "/" my-handler)
(event:run-loop server)  // C++ event loop calls bbl.call per event
```
This requires `bbl.call` but no coroutines.

## Summary: Ranked Improvements

| # | Improvement | Impact | Effort | Needed For |
|---|---|---|---|---|
| 1 | **bbl.call(fn, args)** | Critical | Small | All embedding |
| 2 | **Multiple return values** | High | Medium-Large | ImGui, Physics |
| 3 | **Atomic double-buffer** | High | Small | Audio |
| 4 | **Batch constant registration** | Medium | Trivial | ImGui, Vulkan |
| 5 | **Generational GC** | Medium | Large | Audio, Games |
| 6 | **Coroutines / event loop** | Medium | Large | HTTP |
| 7 | **Mutable references** | Low | N/A | Not needed (tables suffice) |
| 8 | **Call overhead reduction** | Low | N/A | Already fast enough |
