# Inline Caches for Table/Method Access
Status: proposed

## Goal

Reduce table_heavy from 2.4× to ≤1.5× of LuaJIT, and method_dispatch from 2.6× to ≤1.5× of LuaJIT by eliminating C helper call overhead for table/method operations in the per-function JIT.

## Background

### Current bottleneck

Every OP_MCALL emits a C helper call via `emitCallHelper2` — 37 bytes of argument setup + indirect `call rax` + 19 bytes of error check (56 bytes total). The callee (jitTableGet/jitTableSet/jitMcall) then does runtime type dispatch and hash table probing.

For table_heavy (100k get + 100k set), this means 200k C helper calls with full argument marshaling. For method_dispatch (100k iterations × 3 mixed method calls), it's 300k calls through jitMcall's type switch.

### What LuaJIT does

LuaJIT's tracing JIT specializes table access at trace compile time. For tables with known "shapes" (consistent field layouts), it emits inline array/hash lookups with type guards. Method calls to known functions compile to direct calls. No C helper indirection on the hot path.

### BBL table internals

BblTable uses an open-address hash table with linear probing:
- `Entry* buckets` — pointer to entry array (each Entry is 16 bytes: key + val)
- `uint32_t capacity` — always a power of 2
- `uint32_t count` — number of occupied entries
- 75% load factor threshold for resize
- `Entry inlineBuckets[4]` — small table optimization

Hash function: `std::hash<uint64_t>{}(v.bits)` — implementation-defined, not JIT-inlineable.

### NaN-boxing layout

- TAG_OBJECT = 0xFFFF000000000000 — used for tables (and other heap objects)
- TAG_INT = 0xFFFA000000000000
- TAG_STRING = 0xFFFE000000000000
- PAYLOAD_MASK = 0x0000FFFFFFFFFFFF
- Pointer extraction: `bits & PAYLOAD_MASK` (or: `shl 16; shr 16`)
- BblTable, BblVec, BblStruct all share TAG_OBJECT — disambiguation requires checking GcObj::gcType

## Design

### Part 1: JIT-inlineable hash function

Replace `std::hash<uint64_t>{}(v.bits)` in `hashValue()` with a deterministic, JIT-reproducible function:

```cpp
static inline size_t bblHash(uint64_t bits) {
    return bits * 11400714819323198485ULL; // 0x9e3779b97f4a7c15 (Fibonacci/golden ratio)
}
```

In JIT code, this is 2 instructions:
```asm
movabs rcx, 0x9e3779b97f4a7c15
imul rax, rcx
```

Why Fibonacci hash: excellent bit mixing for NaN-boxed values where upper bits are constant (tag) and lower bits vary (payload). Identity hash (`return bits`) would cause clustering for pointer-typed keys where low bits are zero due to alignment.

This change affects all BblTable operations. Correctness is unaffected — hash quality only impacts collision rate, and Fibonacci hash has good distribution properties.

### Part 2: Inline table get (OP_MCALL "get")

For OP_MCALL where the method constant is "get", emit inline x86-64 code that directly probes the hash table. Falls back to C helper on collision or type mismatch.

Fast path (first-probe hit — expected ~75% of accesses):
1. Type guard: check receiver is TAG_OBJECT, extract pointer, check GcObj::gcType == Table
2. Load table->buckets and table->capacity
3. Hash the key: `key.bits * GOLDEN_RATIO`
4. Compute probe index: `hash & (capacity - 1)`
5. Load entry at index (16-byte stride: `buckets + index * 16`)
6. Compare entry.key against search key (64-bit compare on NaN-boxed bits)
7. On match: load entry.val → store to destination register
8. On empty (key == 0): store null to destination
9. On collision: fall through to slow path (existing C helper)

Slow path: call `jitTableGet` as before (handles linear probing, tombstones, non-table receivers).

If the receiver register has a known type (from Part 4), skip the type guard entirely.

### Part 3: Inline table set update (OP_MCALL "set")

For OP_MCALL where the method constant is "set", emit inline code for the UPDATE case (key already exists in table). New insertions require count management, order tracking, and possible resize — these stay in the C helper.

Fast path:
1. Type guard (same as get)
2. Hash + probe first slot (same as get)
3. If entry.key matches: store new value at entry.val — done
4. Otherwise: fall through to slow path (`jitTableSet`)

In table_heavy's second loop (reading 100k existing entries), the get fast path handles all lookups. In the first loop (inserting 100k entries), the set fast path misses (empty slots → slow path) for initial inserts, but subsequent updates to existing keys would hit.

### Part 4: Known-type register tracking

Extend `knownIntRegs` (uint64_t bitmask) to a per-register type enum:

```cpp
enum class KnownType : uint8_t { Unknown, Int, Table, String, Vector };
KnownType knownTypes[256]; // indexed by register number
```

Propagation rules:
- OP_LOADINT, OP_ADDI, OP_SUBI, OP_ADD (both int), OP_SUB (both int), OP_MUL (both int) → KnownType::Int
- OP_TABLE → KnownType::Table
- OP_MOVE → propagate source type
- All other ops → KnownType::Unknown
- OP_CALL, OP_MCALL destination → KnownType::Unknown (return type unknown)

When emitting inline table get/set, check `knownTypes[receiverReg] == KnownType::Table` to skip the type guard. The existing knownIntRegs bitmask is replaced by checking `knownTypes[reg] == KnownType::Int`.

### Part 5: Inline vector:at (OP_MCALL "at" on vectors)

For OP_MCALL "at" when receiver is known/guarded to be a vector:
1. Type guard (if type unknown)
2. Extract BblVec pointer
3. Load vec->elemSize, vec->data pointer, vec->element count
4. Bounds check index (0 ≤ i < count), error on out of bounds
5. Compute offset: `data + index * elemSize`
6. Load 8-byte BblValue at that offset → store to destination

Requires knowing BblVec struct offsets (computed via dummy instance at JIT compile time).

Falls back to C helper for: non-vector receivers, struct-typed elements, negative indices.

### Part 6: Inline string:length (OP_MCALL "length" on strings)

For OP_MCALL "length" when receiver is known/guarded to be a string:
1. Type guard (if type unknown)
2. Extract BblString pointer
3. Load string data size (std::string::size() — implementation detail: on GCC/libstdc++, the size is at offset 8 in the SSO layout)
4. NaN-box as int → store to destination

This replaces a full C call through jitMcall → string method dispatch → size computation.

Note: std::string layout is ABI-specific. Compute the offset via dummy instance at JIT compile time, similar to existing BblClosure::captures offset computation.

### Struct offset computation

All struct offsets are computed at JIT compile time via dummy instances:

```cpp
BblTable dummyTable;
size_t tblBucketsOff = offsetof(BblTable, buckets);   // or via pointer arithmetic
size_t tblCapacityOff = offsetof(BblTable, capacity);

BblVec dummyVec;
size_t vecDataOff = ...;  // offset to data pointer in std::vector<uint8_t>
size_t vecElemSizeOff = ...; // offset to elemSize field
```

GcObj::gcType offset also computed this way. These are stored as static constants computed once at first JIT compilation.

## Considerations

### Code size growth

Each inline table get/set emits ~80-100 bytes vs the current ~56 bytes (helper call + error check). Net increase is ~40 bytes per call site. For a function with 10 OP_MCALL instructions, that's ~400 bytes extra. Acceptable for hot code.

### Hash function change

Changing from `std::hash<uint64_t>` to Fibonacci hash affects all tables globally. Since tables are rebuilt each run (no persistence), this is safe. Performance impact: Fibonacci hash has better distribution than identity hash and comparable quality to the stdlib implementation.

### First-probe miss rate

At 75% load factor with linear probing, the expected number of probes for a successful search is ~2.5 (Knuth). First-probe hit rate is approximately 40-50%. The slow path handles the remaining 50-60%. Even at 40%, this eliminates 40% of C helper calls on the hot path — a significant reduction.

With tombstones from deletions, miss rate increases. But BBL benchmarks don't delete table entries, so this is not a concern for target benchmarks.

### EMPTY_KEY sentinel collision

EMPTY_KEY = 0, which is the same bit pattern as IEEE 754 float 0.0. If a user stores float key 0.0 in a table, `isEmpty()` returns true for an occupied slot. This is a pre-existing limitation in the C table implementation, not introduced by inline caches. The inline cache replicates the same `cmp qword [rdx], 0` check that `isEmpty()` uses, maintaining identical semantics.

### GcObj::gcType check

TAG_OBJECT is shared by tables, vectors, structs, and other heap objects. The type guard must check GcObj::gcType after confirming TAG_OBJECT. This adds one memory load + compare to the guard. Consider: if we already know the type (from knownTypes tracking), we skip this entirely.

### knownIntRegs migration

Replacing `uint64_t knownIntRegs` with `KnownType knownTypes[256]` changes all existing int-check sites from `knownIntRegs & (1ULL << reg)` to `knownTypes[reg] == KnownType::Int`. This is a refactor of existing code — must preserve all current optimizations (tag-preserving arithmetic, tag-aware comparisons).

### std::string ABI dependency

Inlining string:length relies on knowing the offset of the size field in std::string. This is ABI-specific (GCC libstdc++ SSO layout). Computed via dummy instance at JIT compile time, so it adapts to the actual ABI. If the layout changes across compiler versions, the offset computation handles it automatically.

## Acceptance

- table_heavy benchmark ≤1.5× of LuaJIT (currently 2.4×)
- method_dispatch benchmark ≤1.5× of LuaJIT (currently 2.6×)
- All 769 unit tests pass
- All 35 functional tests pass
- No regression on other benchmarks (especially gc_pressure, closure_capture, function_calls which are currently beating or matching LuaJIT)
- knownIntRegs replaced by knownTypes without breaking tag-preserving arithmetic or tag-aware comparisons
