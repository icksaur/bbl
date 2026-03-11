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

### Phase 3: Lightweight child states (~80 LOC)
- Add `parentState` pointer to BblState
- Modify `intern()` to check parent's table first
- Modify child creation to set parentState and share type descriptors
- Share `m.*` method name cache via pointer
- Reduce default VM stack size for children

### Phase 4: Channel as first-class value (~40 LOC)
- `(channel)` creates a MessageQueue-backed userdata
- Methods: `send`, `recv`, `try-recv`, `close`
- Usable with `select`
- Enables communication patterns beyond parent-child

### Phase 5: Lock-free ring buffer (~30 LOC)
- SPSC ring buffer for audio-thread parameter updates
- Power-of-2 capacity, atomic head/tail, zero mutex
- `push` / `pop` both non-blocking, return success bool
- BblValue-typed slots (8 bytes each)
- Registered as `lock-free-ring` userdata

```cpp
struct LockFreeRing {
    std::vector<BblValue> buf;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
    size_t mask;  // capacity - 1

    bool tryPush(BblValue v) {
        size_t h = head.load(std::memory_order_relaxed);
        size_t next = (h + 1) & mask;
        if (next == tail.load(std::memory_order_acquire)) return false;
        buf[h] = v;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(BblValue& out) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        out = buf[t];
        tail.store((t + 1) & mask, std::memory_order_release);
        return true;
    }
};
```

BBL API:
```bbl
(= ring (lock-free-ring 256))
(ring:push 42)           // returns true or false (full)
(= val (ring:pop))       // returns value or null (empty)
(= n (ring:length))      // current element count
```

This completes the audio story: dedicated BblState on audio thread with
GC paused, `lock-free-ring` for parameter updates from main thread,
`atomic-buffer` for sample data output.

```cpp
// C++ audio thread setup
BblState audioBbl;
BBL::addStdLib(audioBbl);
audioBbl.pauseGC();  // never collect on audio thread
audioBbl.exec(R"(
    (= ring (lock-free-ring 64))
    (= buf (vector float32))
    (buf:resize 256)
    (= freq 440.0)
    (= dsp (fn ()
        (= p (ring:pop))
        (if p (= freq p))
        (each i (range 256)
            (buf:set i (* 0.5 (sin (* 2 3.14159 freq
                       (/ (+ offset i) 44100))))))
        (= offset (+ offset 256))))
)");
auto dspFn = audioBbl.get("dsp").value();
auto* ring = /* get ring userdata */;

void audioCallback(float* output, int frames) {
    audioBbl.call(dspFn);
    memcpy(output, audioBbl.getVectorData<float>("buf"), frames * sizeof(float));
}

// Main thread sends param updates via ring (no locks, no BBL interaction)
ring->tryPush(BblValue::makeFloat(880.0));
```

## Test Plan

### Unit Tests (tests/test_bbl.cpp)

**tryRecv:**
- `test_try_recv_empty` — try-recv on child with no messages returns null
- `test_try_recv_has_message` — post to child, child try-recv gets it
- `test_try_recv_preserves_order` — post 3 messages, try-recv returns in order

**select:**
- `test_select_immediate` — post to child before select, returns immediately
- `test_select_two_children` — spawn 2 children, one posts, select returns correct index
- `test_select_timeout_expires` — select-timeout with no messages returns null
- `test_select_timeout_immediate` — select-timeout with ready message returns immediately

**Lightweight children:**
- `test_lightweight_child_basic` — spawn lightweight child, post/recv works
- `test_lightweight_child_intern_shared` — child reads string interned by parent
- `test_lightweight_child_intern_local` — child can intern new strings independently
- `test_lightweight_child_struct_shared` — child uses struct type registered by parent

**Channel:**
- `test_channel_send_recv` — create channel, send value, recv value
- `test_channel_try_recv_empty` — try-recv on empty channel returns null
- `test_channel_with_select` — select on channel + child state
- `test_channel_close` — close channel, recv returns error/null

**Lock-free ring:**
- `test_ring_push_pop` — push value, pop returns same value
- `test_ring_empty_pop` — pop on empty ring returns null
- `test_ring_full_push` — push to full ring returns false
- `test_ring_ordering` — push 10 values, pop in same order
- `test_ring_wrap_around` — push/pop past capacity boundary

### Functional Tests (tests/functional/)

**`concurrent_post_recv.bbl`:**
```bbl
// Parent spawns child, exchanges messages
(= child (state-new "concurrent_child.bbl"))
(child:post (table "msg" "hello"))
(= reply (child:recv))
(assert (== reply.msg "world"))
(child:join)
(print "PASS")
```

**`select_multi_child.bbl`:**
```bbl
// Spawn 3 children that respond at different speeds
(= c1 (state-new "slow_child.bbl"))    // responds after 50ms
(= c2 (state-new "fast_child.bbl"))    // responds after 10ms
(= c3 (state-new "medium_child.bbl"))  // responds after 30ms
// select should return c2 first
(= ready (select c1 c2 c3))
(assert (== ready.index 1))
(print "PASS")
```

**`ring_buffer_threaded.bbl`:**
```bbl
// Main thread pushes to ring, child thread pops
(= ring (lock-free-ring 64))
(= child (state-new "ring_consumer.bbl"))
(child:post (table "ring-ref" 0))  // pass ring via shared mechanism
// push 100 values
(= i 0)
(loop (< i 100)
  (loop (not (ring:push i)) (sleep 0.001))  // retry if full
  (= i (+ i 1)))
(= result (child:recv))
(assert (== result.sum 4950))
(print "PASS")
```

## Acceptance Criteria

1. `select` blocks until any source is ready, returns source index
2. `select-timeout` returns null on timeout
3. `try-recv` returns null immediately if empty
4. Lightweight children share parent's intern table (verified by memory measurement)
5. Lock-free ring: push/pop work across threads without locks
6. No data races (verified by ThreadSanitizer)
7. All existing child-state tests pass
8. All new unit + functional tests pass

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

**Ring buffer GC**: BblValues in the ring may reference GC objects. If the
ring lives on the audio thread (GC paused), those objects must be rooted
elsewhere or be primitive values only. Document that audio-thread rings
should carry primitives (int/float), not GC objects.
