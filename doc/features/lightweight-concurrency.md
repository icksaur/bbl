# Lightweight Concurrency: Select + Cheap Child States

Status: proposed

## Goal

Make BBL's existing actor model (child states + message passing) practical
for real-world concurrency: HTTP servers, audio synthesis, game networking.

Today child states work but are too heavy (~50KB each) and the parent must
busy-poll or block on a single child. This spec adds two features:

1. **`select`** — block until any of N sources has data (channels, timers, sockets)
2. **Lightweight child states** — share intern table + type descriptors with parent (~5KB each)

Together these enable Go-style concurrency: many cheap actors communicating
via channels, multiplexed by `select`, with no shared mutable state.

## Background

### Current Child State System

Each child state is a full `BblState`:
- Own `internTable` (~30 interned method names + all strings)
- Own `structDescs`, `userDataDescs` (re-registered via addStdLib)
- Own `vm->globals` (all stdlib functions re-registered)
- Own GC heap (nurseryHead/tenuredHead)
- Own OS thread via `std::thread`
- Communication via `MessageQueue` (mutex + condvar + deque of `BblMessage`)

Messages carry primitives only (int, float, bool, string, null) + optional
vector/binary payload (zero-copy via `std::move`).

### What Works
- Full isolation — no data races possible
- Message passing is correct and safe
- Payload mechanism gives zero-copy for bulk data

### What Doesn't Scale
- ~50KB per child (intern table, type descs, stdlib globals)
- Only `recv()` (blocking) — no way to wait on multiple sources
- No `tryRecv()` — must use `has-message` + `recv` (race condition window)
- Each child gets its own OS thread (~8MB stack reservation)

## Design

### Part 1: `select` — Multiplexed Waiting

#### API

```bbl
// Block until any source has data. Returns (table "index" N "source" src)
(= ready (select ch1 ch2 ch3))
(= idx ready.index)    // which source (0-based)

// With timeout (milliseconds). Returns null on timeout.
(= ready (select-timeout 100 ch1 ch2))

// Non-blocking poll. Returns null if nothing ready.
(= ready (try-select ch1 ch2))
```

Sources can be:
- Child state handles (wait for message from child)
- Sockets (wait for data available — future extension)
- Timer channels (wait for timeout — future extension)

#### Implementation

Each `MessageQueue` gets a pointer to a shared `selectNotify`:

```cpp
struct SelectNotify {
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> ready{false};
};
```

`select(sources...)`:
1. Create a `SelectNotify` on the stack
2. Register it with each source's MessageQueue
3. Check all sources for existing data (return immediately if found)
4. `cv.wait()` until any source pushes a message
5. Scan sources to find which one(s) are ready
6. Unregister `SelectNotify` from all sources
7. Return the ready source index

`MessageQueue::push()` notifies both its own `cv` AND any registered
`SelectNotify::cv`. Cost: one extra atomic check per push (~1ns).

#### `tryRecv`

Non-blocking receive. Returns null if queue is empty:

```bbl
(= msg (ch:try-recv))
(if msg (handle msg))
```

Implementation: `tryPop()` on MessageQueue — `lock_guard`, check `empty()`,
pop or return null.

### Part 2: Lightweight Child States

#### What to Share

| Resource | Shared? | Mechanism |
|----------|---------|-----------|
| Intern table | ✅ Shared (read-only) | Parent's table, child reads only |
| Type descriptors | ✅ Shared (read-only) | Parent's structDescs/userDataDescs |
| Method name cache (m.*) | ✅ Shared (read-only) | Pointer to parent's m struct |
| Stdlib defn's | ✅ Shared | C function pointers are stateless |
| GC heap | ❌ Independent | Each child has own nursery/tenured |
| VM stack | ❌ Independent | Each child has own register file |
| Globals | ❌ Independent | Script-defined globals are per-child |

The intern table is the biggest win — it contains ~200+ interned strings
after addStdLib. Sharing it eliminates ~15KB per child.

#### Implementation

Add a `parentState` pointer to BblState:

```cpp
struct BblState {
    BblState* parentState = nullptr;  // null for root state

    // Intern: delegate to parent if available
    BblString* intern(const std::string& s) {
        if (parentState) {
            // Check parent's intern table first (read-only, lock-free)
            auto it = parentState->internTable.find(s);
            if (it != parentState->internTable.end()) return it->second;
        }
        // Fall through to own intern table for new strings
        // ...existing code...
    }
};
```

The parent's intern table is populated during addStdLib (before any
children are spawned) and is effectively immutable after that. Children
can read it without locks. New strings interned by children go into
their own local intern table.

Type descriptors and method names: children store a pointer to the
parent's `structDescs`, `userDataDescs`, and `m` struct. These are
populated during setup and never modified at runtime.

#### Estimated Per-Child Cost

| Component | Before | After |
|-----------|--------|-------|
| Intern table | ~15KB | ~0 (shared) |
| Type descriptors | ~5KB | ~0 (shared) |
| Stdlib globals | ~10KB | ~2KB (only defn pointers) |
| VM stack | ~16KB | ~4KB (smaller default) |
| GC heap | ~2KB | ~2KB |
| **Total** | **~50KB** | **~8KB** |

### Part 3: Audio Pattern with Select

The `select` + atomic double-buffer enables a clean audio pattern:

```bbl
(= audio-buf (atomic-buffer "float32" 1024))
(= param-channel (channel))

// DSP loop: fill buffers + handle parameter updates
(loop running
  // Non-blocking check for parameter changes
  (= msg (param-channel:try-recv))
  (if msg (= freq msg.freq))

  // Generate audio
  (= w (audio-buf:write))
  (each i (range 1024)
    (w:set i (* amp (sin (* 2 3.14159 freq (/ (+ offset i) 44100))))))
  (audio-buf:swap)
  (= offset (+ offset 1024))

  // Pace to audio rate
  (sleep-until-swap-consumed audio-buf))
```

C++ audio callback (runs on audio thread, never touches BBL):
```cpp
void audioCallback(float* output, int frames) {
    const float* src = atomicBuf->readPtr();  // lock-free
    memcpy(output, src, frames * sizeof(float));
}
```

For lower latency, the DSP loop can use `select-timeout` to combine
parameter handling with buffer fill timing.

### Part 4: HTTP Server Pattern

```bbl
(= server (tcp-listen 8080))

(= router (table))
(= router.GET (table
  "/" (fn (req) (http:ok "Hello"))
  "/users" (fn (req) (http:ok (json:encode (db:query "SELECT *"))))))

(= children (table))
(= next-id 0)

(loop true
  // Accept new connections (non-blocking via select with server socket)
  (= conn (server:accept))
  (= child (spawn "handler.bbl"))  // lightweight child
  (child:post (table "conn-fd" (conn:fd) "route" (conn:path)))
  (= children.next-id child)
  (= next-id (+ next-id 1))

  // Reap finished children
  (each id (children:keys)
    (= c (children:get id))
    (if (c:is-done) (children:del id))))
```

With lightweight children at ~8KB each, this handles ~1000 concurrent
connections in ~8MB of memory.

## Implementation Plan

### Phase 1: tryRecv + trySelect (~30 LOC)
- Add `MessageQueue::tryPop()` — non-blocking pop
- Register `try-recv` method on State userdata
- Register `try-select` function (poll N sources, return first ready or null)

### Phase 2: select (~60 LOC)
- Add `SelectNotify` struct
- Add `selectNotify` pointer to MessageQueue
- Modify `MessageQueue::push()` to notify select waiter
- Register `select` function (blocking multi-wait)
- Register `select-timeout` function
- Tests: select on 2 children, timeout, immediate availability

### Phase 3: Lightweight child states (~80 LOC)
- Add `parentState` pointer to BblState
- Modify `intern()` to check parent's table first
- Modify child creation to set parentState and share type descriptors
- Share `m.*` method name cache via pointer
- Reduce default VM stack size for children
- Tests: child reads parent's interned strings, child interns new strings
- Benchmark: child creation time and memory

### Phase 4: Channel as first-class value (~40 LOC)
- `(channel)` creates a MessageQueue-backed userdata
- Methods: `send`, `recv`, `try-recv`, `close`
- Usable with `select`
- Enables communication patterns beyond parent-child

## Acceptance Criteria

1. `select` blocks until any source is ready, returns source index
2. `select-timeout` returns null on timeout
3. `try-recv` returns null immediately if empty
4. Lightweight children share parent's intern table (verified by memory measurement)
5. No data races (verified by ThreadSanitizer)
6. All existing child-state tests pass
7. New tests for select, try-recv, lightweight children
8. HTTP server example works with 100+ concurrent connections

## Risks

**Intern table thread safety**: Parent's intern table must be immutable
after child creation. If the parent interns new strings after spawning
children, the children could see a partially-updated hash table. Mitigation:
document that `addStdLib` must be called before spawning children, or add
a read-write lock on the intern table.

**Select scalability**: Scanning N sources on wakeup is O(N). For >1000
sources, consider an `epoll`-style ready queue. For BBL's use case
(tens to hundreds), O(N) scan is fine.

**Channel GC**: Channel userdata holds a MessageQueue. If a channel is
collected while another goroutine is blocked on it, the blocked goroutine
must be woken with an error. Need a finalizer that wakes waiters.
