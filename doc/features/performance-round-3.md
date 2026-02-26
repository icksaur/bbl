# Performance Round 3
Status: done

## Goal

Squeeze the last low-risk gains from the interpreter hot paths, targeting the two compute-heavy benchmarks. Three items: two trivial fast-paths, and one moderate structural change to eliminate wasteful allocation in function calls.

| benchmark | current (round 2) | target | what limits it now |
|-----------|-------------------|--------|--------------------|
| loop_arith | 0.078s | < 0.060s | `resolveSymbol()` on every `set`/`def` target; int→double conversion in `<`/`>` comparisons |
| function_calls | 0.054s | < 0.040s | `unordered_map` default-construction in every `callFn` |

## Design

### Item 0: Cache symbolId on set/def target AstNodes

Every `(= i ...)` and `(= x ...)` calls `resolveSymbol(target.stringVal)` — a `string`-keyed hash lookup — even though the target is an AstNode with a `symbolId` field that could be cached on first use, just like the `eval()` path already does for Symbol nodes.

In the `SpecialForm::Def` handler, replace:
```cpp
uint32_t nameId = resolveSymbol(name.stringVal);
```
With:
```cpp
if (!name.symbolId) name.symbolId = resolveSymbol(name.stringVal);
uint32_t nameId = name.symbolId;
```

Same pattern in the `SpecialForm::Set`/`SpecialForm::Eq` handler for `targetId`.

This saves ~2M string hashes in loop_arith (one per `set i` and `set sum` per iteration).

### Item 1: Integer fast-path for comparison operators

The `CmpLt`/`CmpGt`/`CmpLe`/`CmpGe` handlers unconditionally convert both operands to `double` before comparing. When both are `Int` (the common case in loops), this adds ~2–4ns per comparison for the int→double conversion.

Add an early-out before the float conversion:
```cpp
if (left.type == BBL::Type::Int && right.type == BBL::Type::Int) {
    switch (sf) {
        case SpecialForm::CmpLt: return BblValue::makeBool(left.intVal < right.intVal);
        case SpecialForm::CmpGt: return BblValue::makeBool(left.intVal > right.intVal);
        case SpecialForm::CmpLe: return BblValue::makeBool(left.intVal <= right.intVal);
        case SpecialForm::CmpGe: return BblValue::makeBool(left.intVal >= right.intVal);
        default: break;
    }
}
```

### Item 2: Lazy bindings map initialization in flat-mode scopes

Every `callFn` creates a `BblScope` on the stack, which default-constructs `std::unordered_map<uint32_t, BblValue> bindings`. In libstdc++, this allocates a bucket array (~10 pointers → 80 bytes) on the heap even for an empty map. For flat-mode function call scopes, `bindings` is almost never used — all access goes through `slots`/`slotMap`.

Replace the `bindings` field with `std::unique_ptr<std::unordered_map<uint32_t, BblValue>>`, initialized to `nullptr`. Allocate on first use in `def()` when the flat-mode slot lookup misses (i.e., a new local variable is defined inside the function body that wasn't in the original slot layout).

Changes:
- `BblScope::bindings` → `std::unique_ptr<std::unordered_map<uint32_t, BblValue>> bindings`
- `def()` — allocate map on first bindings use: `if (!bindings) bindings = std::make_unique<...>()`
- `set()` — check `bindings` before accessing: `if (bindings) { auto bit = bindings->find(id); ... }`
- `lookup()` — same null check: `if (bindings) { ... }`
- `gcMarkScope()` — guard iteration with `if (scope.bindings)`
- Root scope: `bindings` is allocated once during `BblState` construction (replace `= default` constructor)
- Direct `rootScope.bindings` accesses in `has()`, `getType()`, `get()`: change `.` to `->` (no null guard needed — rootScope initialization guarantees non-null)

This eliminates 500K malloc/free pairs in function_calls.

## Considerations

- **Capture flat indexing rejected:** Review identified that the self-capture invariant claimed in the original draft is false. Self-capture appends at `slotIndex.size()` = `captures.size() + params.size()`, not `captures.size()`. Flat-index iteration would clobber param slots for recursive functions. Since captures are typically 0–3 entries, the `slotIndex.at()` cost is negligible (~0–60ns per call). Dropped from scope.
- **Const-correctness (Item 0):** `name` and `target` are `const AstNode&` from `node.children`. The `symbolId` field is `mutable uint32_t` so caching works through const refs.
- **Integer precision (Item 1):** For large integers (> $2^{53}$), the int fast-path is strictly more accurate than the double-conversion path, which silently loses precision. This is a correctness improvement, not a risk. Mixed int/float comparisons still go through the double path.
- **Lazy map correctness (Item 2):** Any code path that accesses `bindings` must check for null (in scope methods) or guarantee non-null (rootScope). The three scope methods (`def`, `set`, `lookup`) and `gcMarkScope` are the only indirect access points. Direct `rootScope.bindings` accesses in `has()`, `getType()`, `get()` are safe because rootScope.bindings is always initialized.

## Acceptance

1. All unit tests (309+) and functional tests (17) pass.
2. loop_arith < 0.060s (best-of-5, -O2).
3. function_calls < 0.040s (best-of-5, -O2).
4. No regression in gc_pressure, string_intern, or table_heavy.
5. Results appended to benchmarks.md.
