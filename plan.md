# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

## LSP Tier 2: Runtime-Based Type Analysis

Per `doc/features/lsp-tier2.md`.

### addLspStdLib (src/lsp.cpp)
- [ ] Add `addLspStdLib(BblState&)` function. Register:
  - Real: `BBL::addMath(bbl)`. Register `str`, `typeof`, `int`, `float`, `fmt`, `compress`, `decompress` as the real implementations (copy from addStdLib or call the real defn registrations).
  - Stubbed: `print` → no-op. `fopen`, `filebytes`, `getenv` → push null, return 1. `clock`, `time` → push 0. `sleep`, `exit`, `execute`, `spawn`, `spawn-detached`, `setenv`, `unsetenv`, `chdir`, `mkdir`, `remove`, `rename` → no-op. `state-new` → push null.
  - Real: `execfile` — use the real BblState::execfile so cross-file imports work. Set `allowOpenFilesystem = true` and `scriptDir` to the document's directory.

### Runtime analysis on didOpen/didChange (src/lsp.cpp)
- [ ] In the `didOpen` and `didChange` handlers, after storing the document text:
  1. Create a fresh `BblState lspState`.
  2. Call `addLspStdLib(lspState)`.
  3. Set `lspState.scriptDir` to the directory of the document URI.
  4. Wrap `lspState.exec(text)` in a `std::async` with 500ms timeout.
  5. On success: store the `lspState` (move into a `std::unique_ptr<BblState>`) in the `LspDoc` struct alongside uri/text.
  6. On timeout/exception: store nullptr (fall back to Tier 1 static completions). Report runtime errors as diagnostics (severity=Warning, distinct from parse errors which are severity=Error).
- [ ] Update `LspDoc` struct to hold `std::unique_ptr<BblState> analysis`.

### Enhanced completions (src/lsp.cpp)
- [ ] In `handleCompletion`, after determining `afterColon`:
  - If `afterColon` and the document has a valid analysis state:
    1. Extract the variable name before `:` from the document text (scan backwards from cursor for symbol chars, skip the `:`, read the name).
    2. Look up the variable in `analysis->vm->globals` via `analysis->get(varName)`.
    3. If it's a Table: add its keys from `tbl->order` as completions (kind=Property). Then add standard table methods.
    4. If it's a Vector: add vector methods.
    5. If it's a String: add string methods.
    6. If it's a Binary: add binary methods.
    7. If lookup fails or type unknown: fall back to all methods (existing behavior).
  - If after `(` and analysis exists: add user-defined functions from `vm->globals` (any closure value) as completions with kind=Function.
- [ ] After `.` on a variable (dot access):
  1. Look up the variable. If it's a Struct, look up its `structDescs` entry and add field names as completions.

### Enhanced hover (src/lsp.cpp)
- [ ] In `handleHover`, if the word is not a builtin and analysis exists:
  1. Look up in `analysis->vm->globals`.
  2. If found: show type name + definition info (e.g., "table with keys: x, y, z" or "function(a, b)" or "struct Pixel").
  3. If it's a closure: extract arity from `closureVal()->arity`.

### Timeout wrapper
- [ ] Write a helper `bool execWithTimeout(BblState& state, const std::string& text, int ms)` that runs exec in a detached thread. Returns true on success, false on timeout. On timeout, the BblState is abandoned (leaked — acceptable for a 500ms analysis that failed).

### Tests
- [ ] Pipe a JSON-RPC sequence to `bbl --lsp` that: opens a document with `(= t (table "x" 1)) (= f (fn (a) a))`, sends a completion request after `t:`, verifies `"x"` appears in the completion items.
- [ ] Test that a document with `(loop true 1)` doesn't hang — completions still return (Tier 1 fallback).
- [ ] Verify all 736 unit tests + 24 functional tests still pass.
- [ ] Verify no benchmark regressions.
