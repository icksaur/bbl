# Open Filesystem Access
Status: done

## Goal

Allow `execfile` and `filebytes` to access paths outside the script sandbox (absolute paths, parent traversal) when the C++ host explicitly opts in via a `bool` flag on `BblState`.  Default: off in the library, on in the `bbl` binary interpreter.

## Design

### API

Add a public `bool` field to `BblState` (in `bbl.h`, near `scriptDir`):

```cpp
bool allowOpenFilesystem = false;
```

When `true`, the sandbox checks (absolute path rejection and `..` rejection) are skipped for both `execfile` and `filebytes`.  Path resolution still works normally — paths are resolved relative to `scriptDir` if set, or CWD otherwise.

### Code changes

Two functions contain sandbox checks:

1. **`BblState::execfile()`** (~line 2033 in `bbl.cpp`) — member function; checks `is_absolute()` and `find("..")`.

   Gate with `if (!allowOpenFilesystem)`:
   ```cpp
   if (!allowOpenFilesystem) {
       if (fs::path(path).is_absolute())
           throw BBL::Error{"execfile: absolute paths not allowed: " + path};
       if (path.find("..") != std::string::npos)
           throw BBL::Error{"execfile: parent directory traversal not allowed: " + path};
   }
   ```

   Handle absolute path resolution (replaces existing resolution block):
   ```cpp
   if (fs::path(path).is_absolute()) {
       resolved = fs::path(path);
   } else if (scriptDir.empty()) {
       resolved = fs::path(path);
   } else {
       resolved = fs::path(scriptDir) / path;
   }
   ```

   **BBL_PATH**: the `BBL_PATH` fallback only fires when the resolved file doesn't exist. For absolute paths, this is still useful (try the exact path first, fall back to BBL_PATH). No change needed.

2. **`bblFilebytes()`** (~line 2750 in `bbl.cpp`) — free function; accesses state via `bbl->` pointer.

   Gate with `if (!bbl->allowOpenFilesystem)`:
   ```cpp
   if (!bbl->allowOpenFilesystem) {
       if (fs::path(pathStr).is_absolute())
           throw BBL::Error{"filebytes: absolute paths not allowed: " + pathStr};
       if (pathStr.find("..") != std::string::npos)
           throw BBL::Error{"filebytes: parent directory traversal not allowed: " + pathStr};
   }
   ```

   Handle absolute path resolution (replaces existing resolution block):
   ```cpp
   if (fs::path(pathStr).is_absolute()) {
       resolved = fs::path(pathStr);
   } else if (bbl->scriptDir.empty()) {
       resolved = fs::path(pathStr);
   } else {
       resolved = fs::path(bbl->scriptDir) / pathStr;
   }
   ```

3. **`main.cpp`** (~line 135) — after `BBL::addStdLib(bbl);`, set `bbl.allowOpenFilesystem = true;`.

### Binary interpreter default

The `bbl` binary is a developer tool, not an untrusted-script sandbox. It defaults to open access so scripts can use absolute paths and `..` freely, matching typical shell-scripting expectations.

## Considerations

- **Backwards compatible.** Default is `false`, so existing library users see no behavior change.
- **Single flag.** One bool controls both `execfile` and `filebytes`. No per-function granularity needed — the use cases that need open access need it for both.
- **`fopen` is unaffected.** It was already unrestricted (opt-in via `addFileIo`). No change needed.
- **C++ API calls use the same flag.** `bbl.execfile()` from C++ uses the same `execfile()` method, so the flag applies there too. With `false` (the default), the C++ caller is also sandboxed — this was the existing behavior, so no regression. Library users who call `bbl.execfile()` with absolute paths should set `allowOpenFilesystem = true`.
- **security.md discrepancy.** The security spec claims C++ API calls to `execfile` are unsandboxed, but the code applies sandbox checks to all callers. The security doc update should correct this claim and document the new flag.
- **`..` detection is substring-based.** A filename literally containing `..` (e.g. `data..v2.txt`) is a false positive. This is pre-existing behavior and out of scope for this change.

## Risks

- **Accidental enablement.** A library user could set `allowOpenFilesystem = true` without understanding the security implications, exposing the host filesystem to scripts. Mitigation: default is `false`; the flag name clearly communicates its purpose.

## Acceptance

1. Default `BblState` has `allowOpenFilesystem == false`.
2. With `allowOpenFilesystem = false`: `(execfile "/absolute/path")` throws "absolute paths not allowed".
3. With `allowOpenFilesystem = false`: `(execfile "../parent")` throws "parent directory traversal not allowed".
4. With `allowOpenFilesystem = false`: `(filebytes "/absolute/path")` throws "absolute paths not allowed".
5. With `allowOpenFilesystem = false`: `(filebytes "../parent")` throws "parent directory traversal not allowed".
6. With `allowOpenFilesystem = true`: `(execfile "/absolute/path")` attempts to open the file (no sandbox error).
7. With `allowOpenFilesystem = true`: `(execfile "../file")` attempts to open the file (no sandbox error).
8. With `allowOpenFilesystem = true`: `(filebytes "/absolute/path")` attempts to open the file (no sandbox error).
9. With `allowOpenFilesystem = true`: `(filebytes "../file")` attempts to open the file (no sandbox error).
10. With `allowOpenFilesystem = true`: `(filebytes "relative/path")` still resolves relative to `scriptDir`.
11. `bbl` binary interpreter sets `allowOpenFilesystem = true` before executing scripts.
12. Existing tests continue to pass (sandbox tests still work because they use default `BblState`).
