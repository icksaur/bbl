# BBL Concurrency

BBL's concurrency model is actor-based: isolated child states communicate via
message passing.  No shared mutable state, no data races.

---

## Child States

A child state is an independent BBL interpreter on its own thread.  It shares
the parent's intern table and type descriptors (read-only) but has its own
globals, GC heap, and execution stack.

### Creating

```bbl
(= worker (state-new "worker.bbl"))
```

The child executes the script file on a new OS thread.

### Message Passing

```bbl
// Parent sends to child
(worker:post (table "cmd" "process" "data" 42))

// Parent receives from child (blocking)
(= result (worker:recv))
(print result.status)

// Non-blocking receive
(= msg (worker:try-recv))
(if msg (handle msg))
```

Messages are tables with primitive values (int, float, bool, string, null).
Tables, closures, and userdata cannot be sent — they live in a specific
state's GC heap.

### Vector Payloads

Send bulk data as an optional second argument:

```bbl
(= data (vector float32 1.0 2.0 3.0))
(worker:post (table "type" "vertices") data)

// On receiving side:
(= msg (recv))
(= vertices (recv-vec))
```

The vector bytes are moved (zero-copy) through the message queue.

### Lifecycle

```bbl
(worker:join)                    // block until child finishes
(if (worker:is-done) ...)        // poll without blocking
(if (worker:has-error) ...)      // check for unhandled error
(print (worker:get-error))       // get error message
(state-destroy worker)           // terminate and clean up
```

---

## Channels

First-class message queues for communication within or between states.

```bbl
(= ch (channel))

(ch:send 42)
(ch:send "hello")
(ch:send (table "x" 1 "y" 2))

(= val (ch:recv))       // blocking — waits for message
(= val (ch:try-recv))   // non-blocking — returns null if empty
(ch:close)               // wake all blocked receivers
(print (ch:length))      // pending message count
```

Channels carry any BBL value, including tables and closures (within the
same state).  For cross-state communication, use `post`/`recv` instead.

---

## Select

Wait on multiple sources simultaneously.  Returns the index of the first
source with a ready message.

```bbl
(= w1 (state-new "worker1.bbl"))
(= w2 (state-new "worker2.bbl"))

// Block until either worker sends a message
(= idx (select w1 w2))
(if (== idx 0)
    (print "worker1: " (w1:recv))
    (print "worker2: " (w2:recv)))
```

### Variants

```bbl
(select sources...)               // block until any ready
(select-timeout 1000 sources...)  // block up to 1000ms, null on timeout
(try-select sources...)           // non-blocking poll, null if none ready
```

Sources can be child state handles.  The wakeup is efficient — uses
condition variables, no busy-polling.

---

## Lock-Free Ring Buffer

Single-producer single-consumer (SPSC) ring buffer for zero-lock
communication.  Designed for audio-thread parameter updates where
mutex contention is unacceptable.

```bbl
(= ring (lock-free-ring 256))    // capacity must be power of 2

(ring:push 42)                    // returns true (enqueued) or false (full)
(= val (ring:pop))                // returns value or null (empty)
(print (ring:length))             // current queue depth
```

The ring uses atomic head/tail pointers with acquire/release memory ordering.
No locks, no syscalls, no allocation in the push/pop paths.

### Audio Pattern

```bbl
// Main thread: update parameters
(ring:push (table "freq" 880.0 "amp" 0.3))

// Audio thread (separate BblState): read parameters
(= params (ring:pop))
(if params (= freq params.freq))
```

---

## Atomic Double-Buffer

Lock-free buffer swap for producer/consumer patterns.  The producer writes to
one buffer while the consumer reads the other.  Swap is a single atomic
pointer exchange.

```bbl
(= buf (atomic-buffer "float32" 1024))

// Producer: fill write buffer
(= w (buf:write))
(each i (range 1024)
    (w:set i (* 0.5 (sin (* 2 3.14159 440 (/ i 44100))))))
(buf:swap)                        // atomic — consumer now sees new data

// Consumer (C++ audio callback):
// const float* samples = atomicBuf->readPtr();  // lock-free
```

Element types: `"float32"` (8-byte double internally), `"int"`.

---

## nREPL — Live Evaluation

Embed an eval server in your running program.  Connect from VS Code (or any
JSON-RPC client) to evaluate forms in the program's live state.

### Blocking (setup scripts)

```bbl
(import "physics.bbl")
(import "render.bbl")
(nrepl 7888)         // blocks here — accepts eval requests until killed
```

### Non-blocking (game loops)

```bbl
(= repl (nrepl-start 7888))
(loop running
    (nrepl-poll repl)             // drain pending eval requests
    (physics:step dt)
    (render:frame))
```

### Module Targeting

When the editor sends an eval request with a file path, `nrepl-exec`
evaluates the code in that module's environment.  Redefining a function
in a module is immediately visible to all callers:

```bbl
// Editor evaluates in physics.bbl context:
(= gravity -20)    // updates physics module's env table
                    // all (physics:step) calls now use -20
```

### Direct Use

```bbl
(= result (nrepl-exec "(+ 1 2)"))          // eval in root scope
(print result.value)                         // "3"

(= result (nrepl-exec "(gravity)" "physics.bbl"))  // eval in module
```

Returns a table: `{value, output, error}`.  Print output is captured
separately from the return value.

### Editor Discovery

`(nrepl 7888)` and `(nrepl-start 7888)` write `.bbl-nrepl-port` in the
working directory.  The VS Code extension auto-connects when this file
appears.

---

## Lightweight Children

Child states share the parent's intern table and type descriptors (read-only).
This reduces per-child memory from ~50KB to ~8KB, enabling hundreds of
concurrent actors.

The sharing is automatic — `state-new` sets it up internally.  No user
configuration needed.
