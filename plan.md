# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

## phase 3 — structs and vectors

Deliverable: StructBuilder works, struct types usable from scripts, vectors of structs are contiguous and readable from C++ via getVector<T>(). Method dispatch via `.` works for structs (field access) and vectors (push/pop/clear/length/at).

[ ] 1. **Type descriptors & StructBuilder** — Add CType enum, FieldDesc, StructDesc, BblStruct, BblVec types. Add StructBuilder with field<T> and structField. Add registerStruct to BblState. Add BblStruct*/BblVec* to BblValue union + factories.

[ ] 2. **Struct construction & field access** — In evalList, struct type name is a callable constructor. DotAccess on struct reads field. `(set v.x 5.0)` writes field. Composed structs with chained reads.

[ ] 3. **Vectors** — BblVec stores contiguous typed data. Construction via `(vector type elem...)`. Methods: push, pop, clear, length, at. DotAccess on vector dispatches methods. getVector<T> returns typed pointer.

[ ] 4. **Unit tests** — Register vertex struct, construct, read/write fields. Composed triangle struct. Vector creation, push, pop, at, length, clear. Type mismatch errors. getVector<T> from C++.

[ ] 5. **Functional tests** — structs.bbl, vectors.bbl.

[ ] 6. **Validate** — Build, all tests pass.
