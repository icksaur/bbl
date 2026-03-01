# Performance Analysis and Optimization Opportunities

Benchmarks comparing BBL (tree-walk and bytecode) against Lua 5.4 and LuaJIT.
All measurements on the same machine, release builds, median of 3 runs.

---

## Current Results (with computed-goto dispatch)

| Benchmark          | BBL tree | BBL bytecode | Lua 5.4 | LuaJIT | BC vs tree | BC vs Lua |
|--------------------|----------|--------------|---------|--------|------------|-----------|
| loop_arith (1M)    | 58 ms    | 33 ms        | 6 ms    | 2 ms   | 1.8x ↑     | 5.5x slower |
| function_calls (500K) | 44 ms | 24 ms        | 10 ms   | 1 ms   | 1.8x ↑     | 2.4x slower |
| gc_pressure (100K) | 12 ms    | 8 ms         | 7 ms    | 1 ms   | 1.5x ↑     | ~1x       |
| table_heavy (1K)   | 3 ms     | 2 ms         | 1 ms    | 2 ms   | 1.5x ↑     | ~1x       |

Previous tree-walker was 16x slower than Lua 5.4 on loop_arith.
Bytecode brought that gap down to 5.5x.

### Critical finding: locals vs globals

The loop_arith benchmark runs at top level, so all variables (`s`, `i`, `n`)
are stored as globals (hash map lookup on every access).  Wrapping the same
code in a function makes them locals (direct array indexing):

| Version                    | Time  | vs Lua 5.4 |
|----------------------------|-------|------------|
| BBL bytecode (globals)     | 33 ms | 5.5x slower |
| **BBL bytecode (locals)**  | **2 ms** | **3x faster** |
| Lua 5.4                    | 6 ms  | baseline   |
| LuaJIT                     | 2 ms  | 3x faster  |

**When using locals, BBL bytecode matches LuaJIT and beats Lua 5.4 by 3x.**

The remaining 5.5x gap on the standard benchmark is entirely due to
`OP_GET_GLOBAL`/`OP_SET_GLOBAL` going through `unordered_map::find()`.
Dispatch overhead is negligible.

The VM is already well-utilized (86% retiring, near-zero branch misses).  The
remaining gap vs Lua 5.4 is not dispatch overhead — it's per-instruction work.

---

## Optimization Opportunities

### 1. Computed-goto dispatch

**What:** Replace the `switch(op)` dispatch with GCC's `&&label` computed goto.
Each handler ends with `goto *dispatch_table[*ip++]` instead of breaking back
to the switch.

**Why it helps:** Eliminates the switch bounds check, and gives the CPU's
branch predictor a separate indirect branch per opcode (instead of one shared
branch for all opcodes).  Measured 15-30% improvement in dispatch-bound loops
across CPUs (TU Wien threading benchmark data: switch ~5 cycles/dispatch vs
direct ~2 cycles on modern x86).

**Expected gain:** 15-25% on loop_arith and function_calls.

**Complexity:** Low.  Mechanical transformation: build `static void* table[]`,
replace `switch` with labels, add `DISPATCH()` macro.  Keep `#else` fallback
for non-GCC.  ~50 LOC change.

**References:**
- Eli Bendersky, "Computed Goto for Efficient Dispatch Tables" (2012)
- CPython ceval.c uses this, reports 15-20% improvement
- TU Wien threading benchmark: https://www.complang.tuwien.ac.at/forth/threading/

---

### 2. Superinstructions (fused opcodes)

**What:** Identify common opcode pairs/triples and merge them into single
"superinstructions" that do the work of multiple opcodes in one dispatch.

**Common hot sequences in BBL:**

| Sequence | Frequency | Fused opcode |
|----------|-----------|-------------|
| `GET_LOCAL + GET_LOCAL` | very high (binary ops on locals) | `OP_GET_LOCAL2` |
| `GET_LOCAL + CONSTANT + ADD` | loop counters | `OP_ADD_LOCAL_CONST` |
| `SET_LOCAL + POP` | every assignment | `OP_SET_LOCAL_POP` |
| `CONSTANT + ADD` | accumulation | `OP_ADD_CONST` |
| `GET_LOCAL + LT + JUMP_IF_FALSE` | loop conditions | `OP_LOOP_TEST_LOCAL` |

**Why it helps:** Each superinstruction saves 1-2 dispatch cycles plus the
push/pop overhead of intermediate stack operations.  In the loop_arith
benchmark, the hot loop is:
```
GET_GLOBAL sum    GET_GLOBAL i    ADD    SET_GLOBAL sum    POP
GET_GLOBAL i      CONSTANT 1     ADD    SET_GLOBAL i      POP
GET_LOCAL  i      GET_LOCAL  n    LT     JUMP_IF_FALSE
```
Fusing `GET+GET+ADD` and `GET+CONST+ADD` would cut dispatches by ~40%.

**Expected gain:** 20-40% on arithmetic-heavy benchmarks.

**Complexity:** Medium.  Add 5-10 new opcodes to the enum and dispatch loop.
The compiler emits them via a peephole pass (scan bytecode after compilation,
replace recognized patterns).  ~150-200 LOC.

**References:**
- Lua 5.0 paper: uses iABx format to encode operands inline, achieving similar
  effect via register instructions
- CPython "wordcode" pairs similar operand-merging optimizations

---

### 3. Register-based instruction encoding

**What:** Switch from stack-based to register-based bytecode where each
instruction encodes source/destination register indices.  `ADD A B C` means
`R[A] = R[B] + R[C]` in one dispatch.

**Why it helps:** Stack VMs execute ~47% more instructions than register VMs
for the same program (Shi et al., "Virtual Machine Showdown," 2005).  Each
register instruction replaces 3-4 stack operations (push lhs, push rhs,
add, pop result).  Fewer dispatches = fewer cycles.

Lua 5.x uses 32-bit fixed-width instructions with format:
```
iABC:  opcode(7) | A(8) | B(8) | C(8)    -- 3 register operands
iABx:  opcode(7) | A(8) | Bx(17)         -- register + constant/offset
iAsBx: opcode(7) | A(8) | sBx(17)        -- register + signed offset
```

BBL's existing flat-slot scope model maps directly to registers.  Function
parameters and locals are already at known slot indices.

**Expected gain:** 30-50% on all benchmarks.  Would likely match or exceed
Lua 5.4 performance on compute-bound workloads.

**Complexity:** High.  Requires rewriting the compiler's code generation to
perform register allocation (linear scan is sufficient).  New instruction
encoding/decoding.  ~500-800 LOC rewrite of compiler + VM dispatch.

**References:**
- Ierusalimschy et al., "The Implementation of Lua 5.0" (2005)
- Shi et al., "Virtual Machine Showdown: Stack vs. Registers" (2005)

---

### 4. Inline caching for global lookups

**What:** Cache the result of global variable lookups so that repeated accesses
to the same global (e.g., in a loop) skip the hash table lookup.

**How:** Each `OP_GET_GLOBAL` site gets an inline cache slot (a pointer + key).
On first access, fill the cache.  On subsequent accesses, check if the cache
key matches — if yes, return the cached value directly (O(1) pointer deref
instead of O(hash) map lookup).

For BBL, this is especially valuable because the top-level loop_arith
benchmark uses only globals (`sum`, `i`, `n`) and every iteration does
multiple `OP_GET_GLOBAL` + `OP_SET_GLOBAL` through the unordered_map.

**Expected gain:** 20-40% on code using globals in loops.

**Complexity:** Low-Medium.  Add a `GlobalCache` array alongside the Chunk.
Each GET_GLOBAL instruction gets an index into this array.  ~100 LOC.

**References:**
- Inline caching was invented for Smalltalk-80 (Deutsch & Schiffman, 1984)
- Used in all modern JS engines (V8, SpiderMonkey)

---

### 5. Move loop variables to locals

**What:** The compiler currently puts top-level variables into globals (hash
map).  When a loop body references these variables, every access goes through
`OP_GET_GLOBAL` / `OP_SET_GLOBAL` which do hash table lookups.

If the compiler instead allocated top-level variables as locals (using the
value stack with fixed slots), loop bodies would use `OP_GET_LOCAL` /
`OP_SET_LOCAL` — direct array indexing, no hash lookup.

**Why it helps:** GET_LOCAL is a single array index: `frame->slots[slot]`.
GET_GLOBAL does: read constant, extract symId, `unordered_map::find(symId)`,
fallback to `rootScope.lookup()`.  That's 10-50x more work per access.

In loop_arith, the hot loop executes GET_GLOBAL 4 times and SET_GLOBAL 2
times per iteration (1M iterations = 6M hash lookups).

**Expected gain:** 30-50% on loop_arith immediately.  This alone could bring
BBL within 2x of Lua 5.4.

**Complexity:** Very low.  Change the compiler to always allocate variables as
locals at top level (scopeDepth 0), reserving stack slots.  Only use globals
for names registered via `defn()` (C functions) that are never in the
compilation unit.  ~30 LOC change in `emitSetVar` / `emitGetVar`.

---

### 6. Smaller BblValue representation

**What:** Shrink BblValue from 24 bytes (type enum + bool flags + 8-byte
union + padding) to 16 bytes by packing the type tag and flags into a single
byte or using pointer tagging.

```cpp
// Current: 24 bytes
struct BblValue {
    BBL::Type type;  // 4 bytes (enum)
    bool isCFn;      // 1 byte
    bool isClosure;  // 1 byte
    // 2 bytes padding
    union { ... };   // 8 bytes
    // = 16 bytes aligned to 24 with struct padding
};

// Proposed: 16 bytes
struct BblValue {
    uint8_t tag;     // type + flags packed into 1 byte
    uint8_t pad[7];  // or use for small-int optimization
    union { ... };   // 8 bytes
};
```

**Why it helps:** Smaller values = more values per cache line.  The value
stack, constant pools, and capture arrays all get 33% denser.  For a
stack-heavy VM, this directly improves cache hit rate.

**Expected gain:** 10-20% across all benchmarks.

**Complexity:** Medium.  Touch every `makeXxx()` factory and every type check.
~200 LOC of mechanical changes.

---

### 7. NaN-boxing (if 32-bit integers are acceptable)

**What:** Encode all values in 8 bytes by exploiting IEEE 754 NaN space.
Doubles are stored directly.  Pointers, booleans, and null use the NaN
payload.  Used by LuaJIT, SpiderMonkey, JavaScriptCore.

**Trade-off:** Only 32-bit integers fit natively.  BBL currently uses 64-bit
signed integers.  Would need either (a) truncation to 32-bit, or (b) heap-
boxing for large integers.

**Expected gain:** 30-50% if adopted (halves value size, eliminates type-tag
branch for float operations).

**Complexity:** High.  Requires changing the fundamental value representation.
~500+ LOC.  Only worth it if 32-bit integers are acceptable for the use case.

---

## Recommended Priority Order

| # | Optimization | Expected gain | Complexity | LOC |
|---|-------------|---------------|------------|-----|
| 1 | **Top-level locals** | 30-50% | Very low | ~30 |
| 2 | **Computed goto** | 15-25% | Low | ~50 |
| 3 | **Superinstructions** | 20-40% | Medium | ~200 |
| 4 | **Inline caching** | 20-40% | Low-Medium | ~100 |
| 5 | **Smaller BblValue** | 10-20% | Medium | ~200 |
| 6 | **Register VM** | 30-50% | High | ~800 |
| 7 | **NaN-boxing** | 30-50% | High | ~500 |

Items 1-2 alone could bring loop_arith from 5x slower than Lua to ~2x.
Items 1-4 combined could match or exceed Lua 5.4.
Item 6 would likely exceed Lua 5.4 on all benchmarks.

---

## References

1. TU Wien, "Speed of various interpreter dispatch techniques V2"
   https://www.complang.tuwien.ac.at/forth/threading/

2. Bendersky, E. "Computed Goto for Efficient Dispatch Tables" (2012)
   https://eli.thegreenplace.net/2012/07/12/computed-goto-for-efficient-dispatch-tables

3. Nystrom, R. *Crafting Interpreters*, Ch. 30 "Optimization" (2021)
   https://craftinginterpreters.com/optimization.html

4. Ierusalimschy, R. et al. "The Implementation of Lua 5.0" (2005)
   https://www.lua.org/doc/jucs05.pdf

5. Shi, Y. et al. "Virtual Machine Showdown: Stack vs. Registers" (2005)
   VEE'05 Proceedings
