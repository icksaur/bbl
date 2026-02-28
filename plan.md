# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

## legend

[ ] incomplete
[*] complete

---

## Shebang and Standard Streams (spec.md)

### Phase 1: Shebang

[ ] In `bbl.cpp`, function `BblLexer::skipWhitespaceAndComments()` (line ~168), add shebang handling **before** the `while` loop:
    - If `pos == 0 && pos + 1 < len && src[0] == '#' && src[1] == '!'`, advance until `peek() == '\n'` or `pos >= len`, then let the existing loop handle the newline.
[ ] Build and run existing tests — all 533 must pass.

### Phase 2: File read-line method

[ ] In `bbl.cpp`, add `bblFileReadLine` function above `bblFileClose` (~line 3066):
    - Get self (arg 0), validate File userdata, get `FILE* fp`, check not null.
    - Use `fgets(buf, 4096, fp)` in a loop to handle lines longer than 4096 bytes.
    - If `fgets` returns NULL on first call, push null and return (`bbl->pushNull(); return 1;`... actually use `bbl->returnValue = BblValue::makeNull(); bbl->hasReturn = true; return 0;`).  Wait — check how other methods return values.  `bblFileRead` uses `bbl->pushString`.  Use `bbl->pushNull()` for null.
    - Strip trailing `\n` and `\r\n` from the result string.
    - Push the string via `bbl->pushString(result.c_str())`.
[ ] In `addFileIo`, add `.method("read-line", bblFileReadLine)` to the TypeBuilder chain.
[ ] Build and run existing tests — all 533 must pass.

### Phase 3: stdin/stdout/stderr globals and close protection

[ ] In `bbl.cpp`, modify `fileDestructor` (~line 3088): add guard `if (fp && fp != stdin && fp != stdout && fp != stderr)` before `fclose(fp)`.
[ ] In `bbl.cpp`, modify `bblFileClose` (~line 3066): change the `if (fp)` block to `if (fp && fp != stdin && fp != stdout && fp != stderr)` before calling `fclose` and nulling data.
[ ] In `addFileIo` (~line 3090), after `bbl.defn("fopen", bblFopen);`, register three globals:
    ```cpp
    auto* stdinUd = bbl.allocUserData("File", static_cast<void*>(stdin));
    bbl.set("stdin", BblValue::makeUserData(stdinUd));
    auto* stdoutUd = bbl.allocUserData("File", static_cast<void*>(stdout));
    bbl.set("stdout", BblValue::makeUserData(stdoutUd));
    auto* stderrUd = bbl.allocUserData("File", static_cast<void*>(stderr));
    bbl.set("stderr", BblValue::makeUserData(stderrUd));
    ```
[ ] Build and run existing tests — all 533 must pass (existing File tests must still work).

### Phase 4: Tests

[ ] Add shebang tests to `tests/test_bbl.cpp` before `// ========== Main ==========`:
    - `test_shebang_skipped`: `bbl.exec("#!/usr/bin/env bbl\n(= x 42)")` then `ASSERT_EQ(bbl.getInt("x"), 42)`
    - `test_shebang_only_at_start`: `ASSERT_THROW(bbl.exec("(= x 1)\n#!/usr/bin/env bbl"))` — `#` mid-source is an error
    - `test_shebang_empty_after`: `bbl.exec("#!/usr/bin/env bbl\n")` — no throw, no crash
    - `test_shebang_preserves_line_numbers`: exec code with shebang + error on line 2, catch, verify error message contains "line 2"
[ ] Add File read-line tests:
    - `test_file_read_line`: write a file with "aaa\nbbb\nccc\n", open, call `read-line` three times via `(f:read-line)`, verify strings, fourth call returns null
    - `test_file_read_line_empty_lines`: write "a\n\nb\n", verify second read-line returns empty string (not null)
[ ] Add std streams test:
    - `test_std_streams_exist`: create BblState, addStdLib, verify `bbl.getType("stdin") == BBL::Type::UserData`, same for stdout, stderr
    - `test_std_stream_close_noop`: `bbl.exec("(stdin:close)")` — no throw, no crash; then `bbl.exec("(stdout:flush)")` — still works
[ ] Add RUN entries for all new tests in main().
[ ] Build and run — should be 533 + 8 = 541 tests, all passing.

### Phase 5: Documentation

[ ] Update `bbl.md`:
    - In the File I/O section, add `read-line` to the method table.
    - Add `stdin`, `stdout`, `stderr` section explaining they are File globals.
    - Add shebang to the CLI section.
[ ] Update `api.md` if needed (mention std stream globals registered by addFileIo).
[ ] Build and run tests one final time.

---## Phase 4: Documentation

[ ] **Update spec.md** — Replace all method-call dot syntax with colon in examples. Add colon syntax section.

[ ] **Update vector.md** — All method calls use colon. Integer-dot examples stay.

[ ] **Update table.md** — All method calls use colon. Remove method-first resolution section. Add self-passing sugar section.

[ ] **Update structs.md** — Note that colon on struct is reserved for future struct methods.

[ ] **Update colon-syntax.md status** — Change `Status: proposed` to `Status: done`.

---