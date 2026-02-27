# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [code-quality.md](code-quality.md).

---

## open filesystem access

Full spec: `doc/features/open-filesystem.md`

[*] 1. Add `allowOpenFilesystem` field to `BblState`
  - File: `bbl.h` ~line 385, after `std::string scriptDir;`
  - Add: `bool allowOpenFilesystem = false;`

[*] 2. Gate sandbox checks in `BblState::execfile()`
  - File: `bbl.cpp` ~line 2033
  - Wrap the `is_absolute()` and `find("..")` checks in `if (!allowOpenFilesystem) { ... }`
  - Replace the path resolution block to handle absolute paths:
    ```cpp
    if (fs::path(path).is_absolute()) {
        resolved = fs::path(path);
    } else if (scriptDir.empty()) {
        resolved = fs::path(path);
    } else {
        resolved = fs::path(scriptDir) / path;
    }
    ```

[*] 3. Gate sandbox checks in `bblFilebytes()`
  - File: `bbl.cpp` ~line 2755
  - Wrap the `is_absolute()` and `find("..")` checks in `if (!bbl->allowOpenFilesystem) { ... }`
  - Replace the path resolution block to handle absolute paths:
    ```cpp
    if (fs::path(pathStr).is_absolute()) {
        resolved = fs::path(pathStr);
    } else if (bbl->scriptDir.empty()) {
        resolved = fs::path(pathStr);
    } else {
        resolved = fs::path(bbl->scriptDir) / pathStr;
    }
    ```

[*] 4. Set flag in `main.cpp`
  - File: `main.cpp` ~line 135, after `BBL::addStdLib(bbl);`
  - Add: `bbl.allowOpenFilesystem = true;`

[*] 5. Add tests in `tests/test_bbl.cpp`
  - Tests (existing sandbox tests already cover `allowOpenFilesystem = false`):
    - `test_open_fs_default_off`: verify `bbl.allowOpenFilesystem == false` on fresh `BblState`
    - `test_execfile_abs_blocked`: default state, `(execfile "/tmp/x.bbl")` throws
    - `test_execfile_dotdot_blocked`: default state, `(execfile "../x.bbl")` throws
    - `test_filebytes_abs_blocked`: default state, `(filebytes "/tmp/x.bin")` throws
    - `test_filebytes_dotdot_blocked`: default state, `(filebytes "../x.bin")` throws
    - `test_execfile_abs_open`: set `allowOpenFilesystem = true`, write a temp .bbl file to /tmp, `(execfile "/tmp/test_xxx.bbl")` succeeds
    - `test_execfile_dotdot_open`: set `allowOpenFilesystem = true`, `(execfile "../<valid>")` does not throw sandbox error (may throw file-not-found, which is OK)
    - `test_filebytes_abs_open`: set `allowOpenFilesystem = true`, write a temp file to /tmp, `(filebytes "/tmp/test_xxx.bin")` succeeds
    - `test_filebytes_dotdot_open`: set `allowOpenFilesystem = true`, `(filebytes "../<nonexistent>")` does not throw sandbox error (may throw file-not-found)
  - Register all in `main()` with `--- open filesystem ---` section header.

[*] 6. Build and run full test suite
  - `cmake --build build -j$(nproc) && ./build/bbl_tests`
  - All tests pass, 0 failures.

[*] 7. Update documentation
  - `doc/features/security.md`: Add a section describing `allowOpenFilesystem` flag and its effect. Fix the C++ API claim.
  - `doc/bbl.md`: Note the flag in the `exec / execfile` section.
  - `doc/features/open-filesystem.md`: Update status to `done`.
  - `doc/backlog.md`: Remove the open filesystem access entry.
