# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

## phase 5 — binary data, GC, userdata, and stdlib

Deliverable: GC runs correctly. TypeBuilder enables userdata. File I/O and math stdlib complete.

[ ] 1. **Binary C++ API** — Add `getBinary(name)` and `setBinary(name, ptr, size)` to BblState. Add `filebytes` C function (reads file into BblBinary, sandboxed same as execfile). Register via `addFileIo`.

[ ] 2. **GC mark-and-sweep** — Add `bool marked` to BblBinary, BblFn, BblVec, BblStruct, BblTable. Strings are interned and never collected (per string.md). Add `gc()` method to BblState. Mark phase: trace from rootScope, callArgs, returnValue. Sweep phase: delete unmarked, remove from pools. Trigger: call gc() after every N allocations (configurable). ~BblState does final cleanup (existing behavior).

[ ] 3. **TypeBuilder & userdata** — Add BblUserData struct (typeName, void* data, destructor fn ptr, methods map). Add UserDataDesc (methods map, destructor). Add TypeBuilder class (name, method, destructor, register). Method dispatch via DotAccess. pushUserData C++ API. GC calls destructor on sweep.

[ ] 4. **File I/O — fopen & File type** — Register File as userdata via TypeBuilder. fopen(path) / fopen(path,mode). File methods: read, read-bytes, write, write-bytes, close, flush. Destructor closes handle. fopen is NOT sandboxed. Register via `addFileIo`.

[ ] 5. **addMath** — sin, cos, tan, asin, acos, atan, atan2, sqrt, abs, floor, ceil, min, max, pow, log, log2, log10, exp. Constants pi, e. Int args promoted to float. sqrt of negative → error.

[ ] 6. **Print formatting** — Update bblPrint for richer type display: `<struct TypeName>`, `<vector T length=N>`, `<table length=N>`.

[ ] 7. **Unit tests** — Binary getBinary/setBinary. GC: create objects, drop refs, gc(), no crash; closures survive; stress test. TypeBuilder: register type, call method, destructor on gc. File I/O: write/read/read-bytes/write-bytes. Math: sqrt, sin, abs, pi, e. filebytes sandboxed. addStdLib idempotent.

[ ] 8. **Functional tests** — binary_literal.bbl, math.bbl, file_io.bbl. Validate: build, all tests pass.
