# BBL Performance Analysis

## Benchmark Results

All benchmarks run on the same machine, Release build (`-O2 -g -fno-omit-frame-pointer`).

### BBL vs Lua 5.4 vs LuaJIT

| Benchmark | Iterations | BBL (ms) | Lua 5.4 (ms) | LuaJIT (ms) | BBL/Lua ratio |
|---|---|---|---|---|---|
| loop_arith | 5M | 330 | 23 | 4 | **14x slower** |
| function_calls | 2M | 191 | 37 | 3 | **5x slower** |
| string_intern | 10K | 30 | 2 | 5 | 15x slower |
| table_heavy | 1K | 3 | 1 | 1 | ~3x slower |

### Hardware Counter Comparison (loop_arith 5M, cpu_core)

| Metric | BBL | Lua 5.4 | Ratio |
|---|---|---|---|
| Instructions | 6,506M | 705M | **9.2x more** |
| Cycles | 1,522M | 123M | 12.4x more |
| Branches | 1,396M | 103M | 13.5x more |
| Branch misses | 9,839 | 2,061 | ~same (negligible) |
| IPC | 4.27 | 5.73 | Lua slightly better |
| Time | 310 ms | 28 ms | 11x slower |

**Key insight**: BBL executes **9.2x more instructions** than Lua 5.4 for the same workload. Cache miss rates are ~0% for both. Branch prediction is excellent for both. The bottleneck is pure instruction count.

## Why BBL Is Slower: Tree-Walking vs Bytecode

### BBL's Approach (Tree-Walking Interpreter)

BBL walks the AST tree at runtime. For each iteration of `(loop (< i n) (= sum (+ sum i)) (= i (+ i 1)))`, the interpreter performs:

1. **Condition `(< i n)`**: `evalList()` → switch on cached SpecialForm → `eval(i)` [function call → switch on NodeType → scope.lookup via hash table] → `eval(n)` [same] → integer compare → return BblValue
2. **Body `(= sum (+ sum i))`**: `evalList()` → SpecialForm::Eq → `eval(RHS)` → `evalList(+ sum i)` → SpecialForm::Add → string compare `op == "+"` → `eval(sum)` [lookup] → `eval(i)` [lookup] → type check → add → return BblValue → scope.set [hash table write]
3. **Body `(= i (+ i 1))`**: same pattern
4. Plus `checkTerminated()` and `checkStepLimit()` per body statement

Per iteration: **~6 evalList calls, ~8 eval calls, ~6 hash table lookups, ~2 hash table writes, ~2 string comparisons**.

Each `eval()` call involves: function prologue/epilogue, switch dispatch through 10 NodeType cases, AstNode field access (128+ byte struct), BblValue construction/return. Each scope lookup is an `unordered_map<uint32_t, BblValue>::find()` — hash, bucket chase, key compare, pointer return.

**Estimated instructions per iteration: ~1,300** (6.5B total / 5M iterations)

### Lua 5.4's Approach (Bytecode VM)

Lua compiles source to bytecode first, then executes in a register-based VM. The same loop compiles to:

```
5   LT    2 0 0    ; if i < n, skip next
6   JMP   5        ; exit loop
7   ADD   1 1 2    ; sum = sum + i  (register add)
8   MMBIN 1 2 6    ; metamethod fallback (skipped for ints)
9   ADDI  2 2 1    ; i = i + 1  (add immediate constant!)
10  MMBINI 2 1 6 0 ; metamethod fallback (skipped for ints)
11  JMP   -7       ; loop back
```

The `ADD` instruction's fast path (from `lvm.c`):
```c
#define op_arith(L,iop,fop) {
  TValue *v1 = vRB(i);            // direct register pointer (base + offset)
  TValue *v2 = vRC(i);            // direct register pointer
  if (ttisinteger(v1) && ttisinteger(v2)) {   // tag check (1 comparison)
    lua_Integer i1 = ivalue(v1);
    lua_Integer i2 = ivalue(v2);
    pc++; setivalue(s2v(ra), iop(L, i1, i2));  // result = a + b
  }
  else op_arithf_aux(L, v1, v2, fop);  // float fallback (never taken)
}
```

Key differences:
- **No function calls** — everything happens in one giant `switch` in `luaV_execute`
- **Register-based** — operands accessed by `base + offset` (pointer arithmetic), no hash lookup
- **ADDI instruction** — `i + 1` is a special opcode with the constant baked into the instruction
- **No string comparisons** — opcode dispatch is an integer switch (or computed goto)
- **No intermediate BblValue construction** — values read/written directly in the register file
- **No per-iteration safety checks** — hooks checked once per dispatch via trap flag

**Estimated instructions per iteration: ~140** (705M total / 5M iterations)

## Profiling Breakdown (BBL, loop_arith 5M)

| Function | Self % | What it does |
|---|---|---|
| `evalList` | 40.8% | Switch dispatch, special form lookup, arg evaluation |
| `eval` | 28.2% | NodeType switch dispatch, symbol lookup delegation |
| `BblScope::lookup` | 13.1% | `unordered_map<uint32_t, BblValue>::find()` in root scope |
| `std::operator==(string, char*)` | 4.9% | `op == "+"` string comparison in Add handler |
| `BblScope::set` | 2.7% | `unordered_map` write in root scope |
| `checkStepLimit` | 2.4% | Per-statement safety check (has throw, won't inline) |

## Proposed Optimizations

### Optimization 1: Replace `op == "+"` with enum comparison

**Status**: Not implemented

The Add/Sub/Mul/Div/Mod handler compares `op == "+"` (a `std::string` vs `const char*`) to check for the string concatenation path. But `sf` already holds `SpecialForm::Add`. Use that instead.

```cpp
// Before (5% of loop_arith time):
if (op == "+" && left.type == BBL::Type::String) {

// After (single integer comparison):
if (sf == SpecialForm::Add && left.type == BBL::Type::String) {
```

**Expected impact**: ~5% of loop_arith. One line change, zero risk.

### Optimization 2: Flat vector root scope

**Status**: Not implemented

The root scope uses `unordered_map<uint32_t, BblValue>` for variable storage. Symbol IDs are dense sequential uint32_t starting at 1, making them ideal for flat vector indexing.

Replace hash table lookups (`~20 instructions`) with indexed vector access (`~3 instructions`):

```cpp
struct BblScope {
    // New: flat mode for root scope
    std::vector<BblValue> flatSlots;
    std::vector<uint8_t> flatPresent;
    bool useFlat = false;
    // ... existing fields
};
```

Enable on rootScope at construction, keep existing hash-map path for child scopes.

**Expected impact**: ~8-10% of loop_arith (13% of time is in `lookup`). ~50 lines of change.

### Optimization 3: Split throw from checkStepLimit/checkTerminated

**Status**: Not implemented

These functions contain `throw` statements, preventing the compiler from inlining. Move the throw to a `[[gnu::cold, gnu::noinline]]` helper:

```cpp
[[gnu::noinline, gnu::cold]] void throwStepLimitExceeded();

void checkStepLimit() {
    if (maxSteps && ++stepCount > maxSteps) [[unlikely]]
        throwStepLimitExceeded();
}
```

**Expected impact**: ~2% of loop_arith. ~10 lines of change, trivial.

### Optimization 4: Superinstruction fast path for `(op Symbol Symbol)`

**Status**: Not implemented

Detect the common pattern `(+ sym sym)` / `(< sym sym)` and execute it inline without `eval()` calls — just direct scope lookups + arithmetic:

```cpp
case SpecialForm::Add: /* ... */ {
    // Fast path: (op Symbol Symbol) with int operands
    if (node.children.size() == 3) {
        auto& ln = node.children[1];
        auto& rn = node.children[2];
        if (ln.type == NodeType::Symbol && rn.type == NodeType::Symbol) {
            if (!ln.symbolId) ln.symbolId = resolveSymbol(ln.stringVal);
            if (!rn.symbolId) rn.symbolId = resolveSymbol(rn.stringVal);
            BblValue* lv = scope.lookup(ln.symbolId);
            BblValue* rv = scope.lookup(rn.symbolId);
            if (lv && rv && lv->type == BBL::Type::Int && rv->type == BBL::Type::Int) {
                switch (sf) {
                    case SpecialForm::Add: return BblValue::makeInt(lv->intVal + rv->intVal);
                    // ... other ops
                }
            }
        }
    }
    // ... fall through to general path
}
```

Also handle `(op Symbol IntLiteral)` for `(+ i 1)`.

**Expected impact**: ~5-8% of loop_arith (eliminates 2 function calls per arithmetic op). ~60 lines.

### Combined Expected Impact

| # | Optimization | Effort | loop_arith | function_calls |
|---|---|---|---|---|
| 1 | `sf == Add` instead of `op == "+"` | 1 line | -5% | -4% |
| 2 | Flat vector root scope | ~50 lines | -8% | -3% |
| 3 | Split throw from checkStepLimit | ~10 lines | -2% | -1% |
| 4 | Superinstruction fast paths | ~60 lines | -5% | -2% |
| | **Combined** | ~120 lines | **~15-20%** | **~8-12%** |

This would bring loop_arith from ~330ms to ~265-280ms — still ~10x slower than Lua 5.4.

### The Fundamental Gap

Even with all optimizations, a tree-walking interpreter cannot match a bytecode VM:

- **BBL**: ~1,300 instructions/iteration → optimized ~1,050 instructions/iteration
- **Lua 5.4**: ~140 instructions/iteration

The 7-10x gap is architectural. Lua's advantage comes from:
1. **Bytecode compilation** — parse once, execute compact integer opcodes
2. **Register-based VM** — operands are array offsets, not hash table lookups
3. **Specialized opcodes** — `ADDI` for add-immediate, `LTI` for compare-immediate
4. **No function call overhead per op** — one giant switch loop, no eval/evalList recursion
5. **No intermediate value construction** — values live in a flat register file

Closing this gap would require a bytecode compiler — a separate, significantly larger project.

## What Didn't Work (Previously Tried)

These optimizations were implemented, benchmarked, and **reverted** because they produced minimal or negative results:

1. **FlatSlotMap** (sorted vector instead of unordered_map for function scope slotIndex) — loop_arith doesn't use function scopes. AstNode size bloat caused cache regression.
2. **Slot buffer pooling** (reuse `callFn` scope slot vectors) — Negligible allocation savings.
3. **`__builtin_expect` on checkStepLimit** — Branch prediction already at 0.001%. No effect.
4. **Cached struct-ctor check** (AstNode field) — Added AstNode bloat for negligible gain.

**Lesson**: With 0% cache miss rates and 4.2+ IPC, micro-optimizations that don't reduce instruction count are useless. The only lever is reducing the number of instructions in the hot path.
