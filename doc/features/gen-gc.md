# Generational Garbage Collector for BBL

Status: proposed

## Goal

Replace BBL's stop-the-world mark-sweep GC with a two-generation collector
to achieve bounded pause times for real-time embedding (games, audio, GUI).

**Current state**: Mark-sweep scans ALL live objects every collection.
A program with 10,000 long-lived objects and 50 short-lived temps per frame
marks all 10,050 objects to collect 50. Pause time is proportional to total
heap size, not garbage volume.

**Target**: Gen0 (nursery) collections complete in < 100µs regardless of
total heap size. Full (Gen1) collections are rare and can be scheduled
during safe points (loading screens, level transitions).

## Background

### Current GC Architecture

**Object layout** (bbl.h:74-78):
```cpp
struct GcObj {
    GcType gcType;
    bool marked = false;
    GcObj* gcNext = nullptr;
};
```
All GC-managed types (BblString, BblTable, BblVec, BblClosure, BblFn,
BblBinary, BblStruct, BblUserData) inherit from GcObj.

**Allocation** (bbl.cpp:699-795): Each `alloc*()` method creates a new object,
prepends it to `gcHead` (intrusive linked list), and increments `allocCount`.
Tables use `tableSlab`, closures use `closureSlab`. When `allocCount >= gcThreshold`,
a full collection is triggered.

**Roots** (bbl.cpp:873-902): 8 root categories:
1. `callArgs` vector
2. `returnValue`
3. `lastRecvPayload`
4. `vm->stack` (register file)
5. `vm->frames` (call stack closures)
6. `vm->globals` (symbol table)
7. `currentEnv` (module environment)
8. `moduleCache` (imported modules)

**Mark phase** (bbl.cpp:802-869): Recursive traversal from roots. Tables
mark all array elements + hash bucket entries. Closures mark captures.

**Sweep phase** (bbl.cpp:904-949): Walk `gcHead` linked list, delete
unmarked objects. Reset marks. Set `gcThreshold = max(4096, liveCount * 2)`.

**Slab allocators**: `SlabAllocator<BblTable>` and `SlabAllocator<BblClosure>`
allocate in 256-object slabs with free lists. Other types use raw `new/delete`.

### Write Barrier Sites

A generational GC needs write barriers wherever an old-generation object
can be modified to point to a new-generation object. In BBL:

**Cold path (C helper functions)**:
- `BblTable::set` (bbl.cpp:99-123) — array and hash assignments
- `jitSetField` / `jitSetIndex` (jit.cpp:551-589) — JIT C helpers
- `jitSetCapture` (jit.cpp:150-155) — closure capture writes
- `jitClosure` (jit.cpp:157-180) — capture initialization
- `jitTable` (jit.cpp:183-188) — table construction
- `jitMcall` table.set/push (jit.cpp:215-225) — method dispatch

**Hot path (JIT-emitted native x86-64)**:
- Inline table set (jit.cpp:2536-2670) — direct `mov [rcx+rsi*8], rax`
  to array slot. **CRITICAL**: must add barrier instruction inline.
- Inline array set in trace compiler (jit.cpp:~3500) — same pattern.

**Interpreter (vm.cpp)**:
- OP_SETFIELD, OP_SETINDEX, OP_SETCAPTURE, OP_CLOSURE, OP_TABLE

The JIT inline table set is the most sensitive — it's on the hottest path
and emits raw memory stores. A write barrier here costs 2-3 instructions
(check generation, conditional card/remembered-set update).

## Design

### Two Generations: Nursery (Gen0) + Tenured (Gen1)

**Gen0 (nursery)**: Objects allocated since last Gen0 collection. Small,
fast to scan. Survivors promoted to Gen1.

**Gen1 (tenured)**: Long-lived objects. Only collected during full GC.

### Object Layout Change

Add a generation bit to GcObj:

```cpp
struct GcObj {
    GcType gcType;
    bool marked = false;
    bool old = false;       // false = Gen0 (nursery), true = Gen1 (tenured)
    bool dirty = false;     // write barrier dedup: in remembered set?
    GcObj* gcNext = nullptr;
};
```

3 bytes added but all fit in the existing 6-byte padding gap between
`marked` and `gcNext`. Size remains 16 bytes. No alignment change.

### Remembered Set (Card Table)

Use a **remembered set** to track Gen1 objects that point to Gen0 objects.
When a write barrier fires (old object → new value), add the old object
to the remembered set.

```cpp
// In BblState:
std::vector<GcObj*> rememberedSet;
```

Alternative: a **card table** (byte array indexed by object address >> 9)
for O(1) barrier checks. But BBL objects aren't in a contiguous heap
(they're malloc'd individually), so a card table doesn't map naturally.
A simple vector-based remembered set is the right choice.

### Write Barrier

A write barrier checks: "is the container old AND the value a young GC object?"

Four NaN-boxing tags are GC pointers: TAG_CLOSURE (3), TAG_FN (5),
TAG_STRING (6), TAG_OBJECT (7). TAG_CFN (4) is a raw C function pointer,
NOT a GcObj — must be excluded.

```cpp
inline bool isGcPointer(BblValue v) {
    if (v.isDouble()) return false;
    uint64_t tag = (v.bits >> 48) & 7;
    return tag >= 3 && tag != 4;  // CLOSURE, FN, STRING, OBJECT but not CFN
}

inline GcObj* toGcObj(BblValue v) {
    return reinterpret_cast<GcObj*>(v.bits & BblValue::PAYLOAD_MASK);
}

inline void writeBarrier(BblState* state, GcObj* container, BblValue newVal) {
    if (!container->old) return;            // young container — no barrier
    if (!isGcPointer(newVal)) return;       // not a GC object
    GcObj* obj = toGcObj(newVal);
    if (obj->old) return;                   // both old — no barrier
    if (!container->dirty) {                // dedup via dirty bit
        container->dirty = true;
        state->rememberedSet.push_back(container);
    }
}
```

Uses a `dirty` bit on GcObj (offset 3, fits in existing padding) to
prevent duplicate remembered-set entries when the same old object gets
multiple writes per cycle.

**Nursery allocation exemption**: Stores into newly-allocated Gen0 objects
do NOT need barriers. Only stores into Gen1 objects (container->old) trigger
the barrier.

**For the JIT inline table set** (hot path), emit native x86-64:

```asm
; After: mov [rcx+rsi*8], rax    ; table.array[idx] = val
; Barrier fast path:
test byte [container+2], 1          ; offsetof(GcObj::old) = 2
jz .no_barrier                      ; young container → skip (99% case)
; Slow path: call C helper to check value + update remembered set
; (handles TAG_CFN exclusion, dirty bit, remembered set push)
mov rdi, container
mov rsi, rax
call writeBarrierHelper
.no_barrier:
```

Fast path: 2 instructions, 1 branch (predicted not-taken). The slow path
(C helper call) fires rarely — only when an old container gets a new value.
Cost: ~1-2 ns per table write on the fast path.

**Write barrier sites** (all must call `writeBarrier`):

Cold path (C helpers):
- `BblTable::set` (bbl.cpp:99-123) — array and hash assignments
- `jitSetField` / `jitSetIndex` (jit.cpp:551-589)
- `jitSetCapture` (jit.cpp:150-155)
- `jitClosure` (jit.cpp:157-180) — capture initialization
- `jitTable` (jit.cpp:183-188) — table construction
- `jitMcall` table.set/push (jit.cpp:215-225)
- `jitEnvSet` (jit.cpp:707-725) — environment table writes

Hot path (JIT native):
- Inline table set (jit.cpp:2536-2670) — direct array store
- Inline array set in trace compiler

Interpreter (vm.cpp):
- OP_SETFIELD, OP_SETINDEX, OP_SETCAPTURE, OP_CLOSURE, OP_TABLE, OP_ENVSET

### Collection Algorithm

**Gen0 collection** (frequent, fast):

1. Mark from roots — using a **generation-aware mark** that stops traversal
   at Gen1 objects (they're tracked by the remembered set instead). A Gen1
   object encountered during marking is not recursed into.
2. Mark from remembered set — for each dirty Gen1 object, scan its children
   and mark any Gen0 objects they reference.
3. Sweep ONLY nurseryHead list (skip Gen1 objects entirely).
4. Promote survivors: set `obj->old = true`, move from nurseryHead to
   tenuredHead. Promotion does NOT require pointer updating.
5. Rebuild remembered set: re-evaluate dirty Gen1 objects — those still
   pointing to Gen0 objects stay in the set, others are cleared.
6. Clear all `dirty` flags.

Gen0 only scans nursery objects + remembered set entries. If nursery has
50 objects and remembered set has 10 entries, mark phase touches ~60 objects
regardless of Gen1 size.

**Gen1 collection** (rare, expensive):

Same as today's full mark-sweep over both lists. Triggered when Gen1
grows past `gen1Threshold`, or explicitly via `bbl.gcFull()`.

### Allocation Changes

Objects start as Gen0 (`old = false`). `allocCount` tracks Gen0 allocations.
When `allocCount >= gen0Threshold`, run Gen0 collection.

```cpp
void maybeGC() {
    if (allocCount >= gen0Threshold) {
        collectGen0();
    }
}
```

`gen0Threshold` is small (e.g., 256-1024) to keep nursery collections fast.
`gen1Threshold` is large (e.g., `liveCount * 2` as today) for full collections.

### Linked List Partitioning

Currently all objects are on one `gcHead` list. For generational GC, use
two lists:

```cpp
GcObj* nurseryHead = nullptr;  // Gen0 objects
GcObj* tenuredHead = nullptr;  // Gen1 objects
```

New allocations prepend to `nurseryHead`. Promotion moves objects from
`nurseryHead` to `tenuredHead`. Gen0 sweep only walks `nurseryHead`.
Gen1 sweep walks both lists.

### Public API

```cpp
void gcMinor();                // Force Gen0 collection
void gcFull();                 // Force full collection (Gen0 + Gen1)
void gc();                     // Same as gcMinor() (backward compatible)
size_t gcGen0Count() const;    // Number of Gen0 objects
size_t gcGen1Count() const;    // Number of Gen1 objects
```

### Slab Allocator Integration

Tables and closures use slab allocators. Slab-allocated objects need the
same `old` flag. No structural change needed — the `old` field is on GcObj
which slab objects inherit from. The slab free list must handle both
generations (freed Gen1 objects return to the slab free list normally).

### Intern Table Interaction

Interned strings are NOT unconditionally rooted — when swept (unmarked),
they're erased from `internTable` (bbl.cpp:912-914). They survive only if
reachable from actual roots. In practice, interned strings used by globals
or closures will be promoted to Gen1 on first Gen0 collection and stay
long-lived. Rarely-used interned strings may be collected and re-interned
later. This is correct behavior — no special handling needed.

### sliceCache

`sliceCache` (bbl.h:584-586) holds `BblString*` pointers. Must be cleared
during Gen0 collection (as it already is during full GC at bbl.cpp:948)
to prevent dangling references to collected Gen0 strings.

### pauseGC Interaction

`pauseGC()` must prevent BOTH Gen0 and Gen1 collections. Either set
both `gen0Threshold = SIZE_MAX` and `gen1Threshold = SIZE_MAX`, or use
a single `gcPaused` boolean flag checked before any collection.

## Implementation Plan

### Phase 1: Infrastructure (~50 LOC)
- Add `old` field to GcObj
- Split `gcHead` into `nurseryHead` / `tenuredHead`
- Add `rememberedSet` vector to BblState
- Update allocation functions to prepend to `nurseryHead`
- Update `gc()` to be `gcMinor()`, add `gcFull()`

### Phase 2: Write Barriers — Cold Path (~30 LOC)
- Add `writeBarrier()` inline function
- Insert barrier in `BblTable::set` (2 call sites)
- Insert barrier in `jitSetField`, `jitSetIndex`, `jitSetCapture`
- Insert barrier in `jitClosure`, `jitTable`
- Insert barrier in `jitMcall` table.set/push paths

### Phase 3: Write Barrier — JIT Hot Path (~40 LOC)
- Emit barrier instructions after inline table set (jit.cpp:~2628)
- Emit barrier instructions after inline array set in trace compiler
- May use a C helper call for the slow path (remembered set push)

### Phase 4: Gen0 Collection (~60 LOC)
- `collectGen0()`: mark from roots + remembered set, sweep nurseryHead only
- Promote survivors: move from nurseryHead to tenuredHead, set `old = true`
- Clear remembered set
- Dynamic `gen0Threshold` sizing

### Phase 5: Testing (~50 LOC)
- Unit test: Gen0 collection collects short-lived objects
- Unit test: Gen1 objects survive Gen0 collection
- Unit test: Write barrier tracks cross-generation references
- Unit test: Promotion works correctly
- Benchmark: gc_pressure with generational vs mark-sweep
- Benchmark: render loop simulation (many short-lived allocations)
- Stress test: remembered set growth under heavy cross-gen writes

## Risks and Mitigations

**Risk: Write barrier overhead on hot paths**
The JIT inline table set is the hottest write path. Adding 6 instructions
per write could regress `table_heavy` benchmark. Mitigation: profile before
and after; if regression > 5%, consider card-marking (single byte store
instead of branch + remembered set push).

**Risk: Remembered set growth**
If many Gen1 objects point to Gen0 objects (e.g., a large long-lived table
with frequently-replaced values), the remembered set could grow large,
making Gen0 collections slow. Mitigation: deduplicate remembered set
entries; promote objects that appear in remembered set frequently.

**Risk: Promotion storm**
If a burst of allocations all survive Gen0 (e.g., building a large data
structure), all get promoted at once, and the remembered set for subsequent
Gen0 collections becomes large. Mitigation: adaptive promotion threshold;
if promotion rate is high, increase gen0Threshold to batch more collections.

**Risk: Breaking existing GcPauseGuard usage**
`GcPauseGuard` sets `gcThreshold = SIZE_MAX`. This should continue to work —
it prevents both Gen0 and Gen1 collections. No change needed.

## Acceptance Criteria

1. Gen0 collection completes in < 100µs for nurseries up to 1000 objects
2. No regression in existing benchmarks (< 5% overhead from write barriers)
3. gc_pressure benchmark improves (fewer full-heap scans)
4. All existing tests pass
5. New tests for generational behavior
6. `GcPauseGuard` continues to work
7. `bbl.call()` works correctly with generational GC

## Estimated Effort

~230 LOC across bbl.h, bbl.cpp, jit.cpp. Medium complexity — the hardest
part is the JIT inline write barrier (Phase 3). Total: 5-8 implementation
hours.

## Alternatives Considered

**Incremental marking**: Mark a few objects per allocation instead of all at
once. Avoids long pauses but adds overhead to every allocation. Doesn't
reduce the total mark work — just spreads it out. Generational is better
because it reduces total work (skip marking long-lived objects).

**Tri-color marking**: Concurrent GC with read/write barriers. Overkill for
BBL's single-threaded model. Adds complexity for no benefit since BBL
can't run user code during GC anyway.

**Region-based allocation**: Allocate nursery objects in a contiguous bump
allocator, free the entire region at once. Very fast allocation (bump
pointer) and collection (reset pointer). But BBL objects vary in size
and some (tables, closures) use slab allocators. Would require significant
refactoring. Good long-term goal but too invasive for now.
