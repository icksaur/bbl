# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [code-quality.md](code-quality.md).

---

## integer dot syntax — `v.0`, `t.0`

Add integer literal after `.` for positional read/write on vectors and tables.
See `doc/features/integer-dot-syntax.md` for full spec.

### [*] 1. Parser: accept Int token after Dot

In `bbl.cpp` `parseExpr`, the dot-access `while` loop (~line 437):

Current code requires `field.type != TokenType::Symbol` → error.

Change: accept both `Symbol` and `Int`.  When `Int`:
- Set `dot.intVal = field.intVal`
- Set `dot.stringVal = ""` (sentinel: empty means integer-dot)

When `Symbol` (existing path): behavior unchanged.

### [*] 2. Eval read: handle integer-dot on Vector and Table

In `bbl.cpp` `eval()`, `case NodeType::DotAccess` (~line 743):

Add an early check **before** any existing type branches:

```cpp
if (field.empty()) {
    int64_t idx = node.intVal;
    if (left.type == BBL::Type::Vector) {
        return readVecElem(left.vectorVal, static_cast<size_t>(idx));
    }
    if (left.type == BBL::Type::Table) {
        return left.tableVal->get(BblValue::makeInt(idx));
    }
    throw BBL::Error{"integer index not supported on " + typeName(left.type)};
}
```

This must come before the existing `if (left.type == BBL::Type::Vector)` branch (which throws "vector methods must be called").

### [*] 3. Place expression write: handle integer-dot on Vector and Table

In `bbl.cpp` `SpecialForm::Eq`, in the `DotAccess` branch (~line 958):

Add a check for `fieldName.empty()`:

```cpp
if (fieldName.empty()) {
    int64_t idx = target.intVal;
    if (obj.type == BBL::Type::Vector) {
        writeVecElem(obj.vectorVal, static_cast<size_t>(idx), val);
        return BblValue::makeNull();
    }
    if (obj.type == BBL::Type::Table) {
        obj.tableVal->set(BblValue::makeInt(idx), val);
        return BblValue::makeNull();
    }
    throw BBL::Error{"cannot set integer index on " + typeName(obj.type)};
}
```

This must come before the existing `if (obj.type == BBL::Type::Struct)` branch.

### [*] 4. Add unit tests

Add to `tests/test_bbl.cpp` after the existing vector set tests:

- `test_int_dot_vector_read` — `(= v (vector int 10 20 30)) (= r v.1)` → 20
- `test_int_dot_vector_write` — `(= v (vector int 10 20 30)) (= v.1 99) (= r v.1)` → 99
- `test_int_dot_vector_struct_chain` — `(= v (vector vertex (vertex 1 2 3))) (= r v.0.x)` → 1.0
- `test_int_dot_vector_out_of_bounds` — `(= v (vector int 1)) v.5` → throws
- `test_int_dot_table_read` — `(= t (table)) (t.push "hello") (= r t.0)` → "hello"
- `test_int_dot_table_write` — `(= t (table)) (t.push "hello") (= t.0 "world") (= r t.0)` → "world"
- `test_int_dot_on_int_error` — `(= x 5) x.0` → throws

Add corresponding `RUN()` lines in `main()`.

### [*] 5. Update docs

- `doc/features/vector.md`: add integer-dot example to element access section
- `doc/features/table.md`: add integer-dot section
- `doc/bbl.md`: add integer-dot examples to Vectors and Tables sections
- `bbl-bench/doc/bbl.md`: mirror `doc/bbl.md` changes
- `doc/features/integer-dot-syntax.md`: set status to done

### [*] 6. Build and run all tests

```
cmake --build build -j$(nproc) && ./build/bbl_tests
cd bbl-bench && bash run.sh ../build/bbl
```

All tests must pass (existing + new).  All 5 bblbench scripts must pass.
