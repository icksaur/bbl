# Bitwise Operations
Status: done

## Goal

Add integer bitwise operators to BBL as special forms: `band`, `bor`, `bxor`, `bnot`, `shl`, `shr`.  These operate on `Int` values only (no float promotion).

## Design

Six new special forms:

| Form | Args | Description | C++ equivalent |
|------|------|-------------|----------------|
| `band` | 2+ | bitwise AND | `a & b` |
| `bor`  | 2+ | bitwise OR  | `a \| b` |
| `bxor` | 2+ | bitwise XOR | `a ^ b` |
| `bnot` | 1   | bitwise NOT (complement) | `~a` |
| `shl`  | 2   | shift left  | `a << b` |
| `shr`  | 2   | shift right (arithmetic) | `a >> b` |

### Implementation approach

Files: `bbl.cpp` (enum ~line 975, lookup table ~line 982, evalCall switch ~line 1360), `tests/test_bbl.cpp`, `doc/bbl.md`.

1. Add six entries to `SpecialForm` enum: `Band`, `Bor`, `Bxor`, `Bnot`, `Shl`, `Shr`.
2. Add six entries to `lookupSpecialForm` table.
3. Add evaluation logic in the `switch(sf)` block of `evalCall`:
   - **`band`, `bor`, `bxor`** — variadic (2+ args), fold left-to-right. Note: existing numeric arithmetic (`+`, `-`, etc.) is binary-only; the bitwise variadic ops need a new fold loop:
     ```
     int64_t result = eval(children[1]).intVal;
     for (i = 2..n) result = result OP eval(children[i]).intVal;
     return makeInt(result);
     ```
   - **`bnot`** — unary (exactly 1 arg), same pattern as `SpecialForm::Not` (~line 1351).
   - **`shl`, `shr`** — binary (exactly 2 args).
4. All six operators require `Int` operands. Floats, strings, etc. produce a type error: `"type mismatch: <op> requires int, got <type>"`.
5. Arity errors use: `"<op> requires at least 2 arguments"` or `"<op> requires exactly N arguments"`.
6. Shift amount must be non-negative and < 64.

### Examples

```bbl
(band 255 15)          // 15
(bor 15 240)           // 255
(bxor 255 15)          // 240
(bnot 0)               // -1
(shl 1 8)              // 256
(shr 256 4)            // 16

// variadic
(band 7 6 5)           // 4
(bor 1 2 4)            // 7

// combining flags
(= flags (bor 1 4))           // 5
(if (!= (band flags 1) 0)
    (print "flag 0 set\n"))
```

Note: BBL has no hex literal support; all examples use decimal.

## Considerations

- **Int-only, no float promotion.** Arithmetic operators promote int to float when mixed. Bitwise operations have no meaningful float interpretation, so mixing int and float is a type error. Same approach as C.
- **Naming.** `band`/`bor`/`bxor`/`bnot` avoids collision with `and`/`or`/`not` (logical). Lua uses the same convention. `shl`/`shr` are clear and short.
- **Variadic fold is new.** Existing numeric arithmetic is binary-only (only string `+` loops). The fold loop for `band`/`bor`/`bxor` is new code, evaluating children 1..N left-to-right.
- **Shift bounds.** Shifting by negative throws an error. Shifting by ≥ 64: `shl` returns 0; `shr` returns 0 for non-negative values and -1 for negative values (consistent with arithmetic shift semantics).
- **`shr` is arithmetic.** C++ `>>` on signed integers is arithmetic shift on all modern platforms (and guaranteed since C++20). `(shr -1 1)` → `-1`. `(shr -1 64)` → `-1`.
- **Special form, not C function.** Keeps bitwise ops consistent with `+`, `-`, etc. and avoids C function dispatch overhead.

## Acceptance

1. `(band 255 15)` → `15`
2. `(bor 1 2 4)` → `7`
3. `(bxor 255 15)` → `240`
4. `(bnot 0)` → `-1`
5. `(shl 1 8)` → `256`
6. `(shr 256 4)` → `16`
7. Variadic: `(band 7 6 5)` → `4`
8. `(bnot -1)` → `0`
9. `(shr -1 1)` → `-1` (arithmetic shift)
10. `(shr -1 64)` → `-1` (arithmetic large shift)
11. `(shl 1 64)` → `0` (large shift)
12. Type error on float: `(band 1.0 2)` throws
13. Type error on string: `(bor "x" 1)` throws
14. Arity error: `(band 1)` throws (needs 2+), `(bnot 1 2)` throws (needs 1), `(shl 1)` throws (needs 2)
15. Shift by negative throws: `(shl 1 -1)` throws
