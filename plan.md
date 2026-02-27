# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [code-quality.md](code-quality.md).

---

## C Function Assignment Safety

Full spec: `doc/features/cfn-assign.md`

### Fix crash

[*] 1. Fix self-capture guard in `=` handler

### Fix semantics

[*] 2. Fix equality comparison for Fn type

### Optional improvement

[*] 3. Improve toString for C functions

### Tests

[*] 4. Add tests in `tests/test_bbl.cpp`

### Verify

[*] 5. Run full test suite — 408 passed, 0 failed

### Finalize

[*] 6. Update `doc/features/cfn-assign.md` status to `done`
