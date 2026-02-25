# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

## phase 6 — CLI completion and security

Deliverable: full CLI (REPL, script args, all options), sandbox enforcement, BBL_PATH, end-to-end functional tests.

[ ] 1. **REPL** — In main.cpp, when no args: enter interactive loop. Prompt `> ` for new, `. ` for continuation. Multi-line via paren counting. Print non-null results. Errors to stderr, continue. Exit on EOF/Ctrl-C.
[ ] 2. **Script arguments** — Inject `args` table into root scope before script runs. `bbl script.bbl arg1 arg2` → `args` = `(table 0 "arg1" 1 "arg2")`. Empty table if no args.
[ ] 3. **Multiple -e flags** — Process all `-e` args in sequence in the same BblState, then exit.
[ ] 4. **BBL_PATH** — In `BblState::execfile`, after sandbox checks, if file not found relative to scriptDir, search directories in `BBL_PATH` env var (colon-separated). First match wins.
[ ] 5. **Unit tests** — REPL paren balance helper (if extracted), args table from CLI script, BBL_PATH resolution, multiple -e, sandbox chain.
[ ] 6. **Functional tests** — args.bbl (print args.length), sandbox tests, BBL_PATH test.
[ ] 7. **Build / test / commit / merge** — All pass, merge to master, mark phase 6 complete.
