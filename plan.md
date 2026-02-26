# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

Spec: `doc/features/performance-improvements.md`

## Phase 1 — Low-risk refactors (no parser changes)

[ ] **Step 1: GC sweep — partition instead of erase**
- In `BblState::gc()` (bbl.cpp ~line 582), replace each sweep loop that uses `vector::erase()` with a partition approach:
  - Use `std::partition` to move marked objects to the front of each pool vector.
  - Delete unmarked objects in the tail.
  - Resize the vector.
  - Clear the marked flag on survivors.
- Repeat for all 6 pool vectors (binaries, fns, structs, vectors, tables, userdatas). Do NOT sweep `allocatedStrings` — interned strings have no `marked` field and are never freed until `BblState` destruction.
- Run all 306 unit tests + 17 functional tests.
- Benchmark gc_pressure before and after.

[ ] **Step 2: Adaptive GC threshold**
- After the sweep in `gc()`, count surviving objects across all pools.
- Set `gcThreshold = std::max(256, liveCount * 2)`.
- No other changes needed — `allocCount` reset already happens.
- Run all tests.
- Benchmark gc_pressure.

[ ] **Step 3: Special-form dispatch table**
- Define a type alias: `using SpecialFormFn = BblValue(*)(BblState&, const AstNode&, BblScope&)` (or use `std::function` if captures needed).
- Extract each special-form body from the `if` chain in `evalList` into standalone functions.
- Build a `static const std::unordered_map<std::string, SpecialFormFn> specialForms` initialized once.
- In `evalList`, after checking `head.type == NodeType::Symbol`, do `auto it = specialForms.find(op)`. If found, call `it->second(...)`. Otherwise fall through to function-call path.
- Verify the actual special forms in `evalList` are covered (15 outer branches):
  `def`, `set`/`=`, `if`, `loop`, `and`, `or`, `fn`, `exec`, `execfile`, `vector`, `table`, `not`, `+`/`-`/`*`/`/`/`%`, `==`/`!=`/`<`/`>`/`<=`/`>=`.
- Note: struct-constructor lookup (`structDescs.find(op)`) remains as a dynamic fallback **before** the user-function call path. `print`, `type`, `len`, `at`, `push`, `pop`, `slice`, `keys`, `values`, `has`, `del`, `to-int`, `to-float`, `to-string` are NOT special forms (they are C functions or method dispatch).
- Run all tests.
- Benchmark loop_arith.

## Phase 2 — Parser-level changes

[ ] **Step 4: Intern string literals at parse time**
- Add an optional `BblString* cachedString = nullptr` field to `AstNode` (for `StringLiteral` nodes).
- In the parser, when creating a `StringLiteral` node, call `state.intern(node.stringVal)` and store in `cachedString`.
  - Parser functions already have access to `BblState*` (parse() is a member).
- In `eval()`, for `NodeType::StringLiteral`: if `node.cachedString` is set, return `BblValue::makeString(node.cachedString)` directly, skip `intern()`.
- Run all tests.
- Benchmark string_intern.

[ ] **Step 5: Symbol IDs**
- Add `uint32_t symbolId` field to `AstNode` (for `Symbol` nodes). Default 0 = unresolved.
- Add `std::unordered_map<std::string, uint32_t> symbolIds` and `uint32_t nextSymbolId = 1` to `BblState`.
- In the parser, when creating a `Symbol` node, resolve name → ID via `symbolIds` and store in `node.symbolId`.
- Change `BblScope::bindings` from `unordered_map<string, BblValue>` to `unordered_map<uint32_t, BblValue>`.
- Change `lookup()`, `set()`, `def()` to take `uint32_t id` (keep string overloads for C++ API that resolve via `BblState`).
- Update all call sites in `eval`/`evalList` to pass `node.symbolId`.
- Add `uint32_t BblState::resolveSymbol(const std::string& name)` helper for C++ embedding API.
- Update ALL public API methods that take string names (`def`, `set`, `get`, `has`, `defn`, `getInt`, `setFloat`, etc. — ~25 methods in bbl.h) to internally call `resolveSymbol()` before delegating to the ID-based scope methods.
- Run all tests.
- Benchmark loop_arith.

[ ] **Step 6: Flat scope for function calls**
- Add `std::vector<uint32_t> slotLayout` to `BblFn` — maps slot index → symbol ID for each capture + param.
- At `fn` creation, compute `slotLayout`: captures first (in order), then params (in order). Store slot count.
- Add flat-mode to `BblScope`: when created with a `slotLayout`, allocate `std::vector<BblValue> slots` and use direct indexing for known IDs. Lookup misses are undefined-symbol errors (function call scopes have no parent — closures see only captures + args). The `bindings` map is not allocated in flat mode.
- In `callFn`, create a flat-mode `BblScope`:
  - Allocate `slots` with the right size.
  - Copy captures by slot index.
  - Bind args by slot index.
- Run all tests.
- Benchmark function_calls.

## Phase 3 — Validation

[ ] **Step 7: Record results**
- Run all 5 benchmarks with `hyperfine --warmup 3`.
- Update the results table in `doc/performance.md`.
- Update status in `doc/features/performance-improvements.md` to `done` (or note partial results).
