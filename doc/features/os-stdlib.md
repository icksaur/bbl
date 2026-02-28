# os Standard Library
Status: implemented

## Goal

Provide operating-system facilities that make BBL useful for general-purpose shell scripting.  Inspired by Lua's `os` library but adapted to BBL's architecture (flat namespace, colon-method syntax, userdata types).  Registered by a new `addOs(BblState&)` function and included in `addStdLib` so the CLI gets it by default.  Embedders can omit it for sandboxed environments.

## Design

### Top-level functions

| Function | Signature | Returns | Description |
|---|---|---|---|
| `getenv` | `(getenv name)` | string / null | Read environment variable. Null if unset. |
| `setenv` | `(setenv name value)` | null | Set environment variable (calls `setenv(3)`). |
| `unsetenv` | `(unsetenv name)` | null | Remove environment variable. |
| `clock` | `(clock)` | float | CPU time used by process in seconds (`clock(3)` / `CLOCKS_PER_SEC`). |
| `time` | `(time)` | int | Current Unix epoch time in seconds (`time(2)`). |
| `sleep` | `(sleep seconds)` | null | Sleep for `seconds` (float). Uses `nanosleep(2)` for sub-second precision. |
| `exit` | `(exit code)` | — | Terminate the process. `code` is an int (default 0). Calls `std::exit`. |
| `execute` | `(execute command)` | int | Run shell command via `system(3)`. Returns exit status. |
| `getpid` | `(getpid)` | int | Current process ID. |
| `getcwd` | `(getcwd)` | string | Current working directory. |
| `chdir` | `(chdir path)` | bool | Change working directory. Returns true on success. |
| `mkdir` | `(mkdir path)` | bool | Create directory. Returns true on success. |
| `remove` | `(remove path)` | bool | Delete file or empty directory. Returns true on success. |
| `rename` | `(rename old new)` | bool | Rename/move file. Returns true on success. |
| `tmpname` | `(tmpname)` | string | Generate a temporary file path via `mkstemp`. Creates the file (caller should `remove` when done). |
| `glob` | `(glob pattern)` | table | Expand a shell glob pattern. Returns a table with integer keys (1-based). Linux: `glob(3)`. |
| `stat` | `(stat path)` | table / null | File metadata: `size`, `mtime`, `is-dir`, `is-file`. Null if path doesn't exist. |

### Process spawning — `spawn` and `spawn-detached`

For shell scripting, the ability to launch child processes and capture their output is critical.

#### `spawn` — capture output (popen-style)

```bbl
(= proc (spawn "ls -la"))       // launches command, returns Process userdata
(= output (proc:read))          // read all stdout as string
(= line (proc:read-line))       // or read line-by-line
(= status (proc:wait))          // wait for exit, returns exit code (int)
```

`spawn` calls `popen(3)` in read mode (read-only — captures child stdout).  Returns a **Process** userdata with methods:

| Method | Returns | Description |
|---|---|---|
| `read` | string | Read all remaining stdout. |
| `read-line` | string / null | Read one line, strip newline. Null at EOF. |
| `wait` | int | Close pipe, wait for process, return exit status. |

The Process destructor calls `pclose` if not already waited.  `kill` is deferred — `popen` does not expose the child PID portably.

#### `spawn-detached` — fire-and-forget background process

```bbl
(= pid (spawn-detached "long-running-server --port 8080"))
(print "launched server as pid " pid "\n")
```

Launches a child process that is **not tied to the BBL script's lifetime**.  The child is fully detached: it survives after the parent script exits.  Implementation uses `fork()` + `setsid()` + `execvp("/bin/sh", "-c", command)` to daemonize the child.  Returns the child PID as an int.  There is no pipe — stdout/stderr of the detached child go wherever they were already pointed (or /dev/null).

This is useful for scripts that need to start background services, daemons, or long-running tasks without blocking.

| `spawn-detached` | `(spawn-detached command)` | int | Launch a detached background process. Returns child PID. Not tied to script lifetime. |

### Date/time formatting

| Function | Signature | Returns | Description |
|---|---|---|---|
| `date` | `(date)` or `(date fmt)` or `(date fmt timestamp)` | string | Format time via `strftime(3)`. Default format: `"%Y-%m-%d %H:%M:%S"`. |
| `difftime` | `(difftime t2 t1)` | float | Difference in seconds between two timestamps. |

### Registration

```cpp
namespace BBL {
    void addOs(BblState& bbl);   // new
}
```

`addStdLib` becomes:

```cpp
void BBL::addStdLib(BblState& bbl) {
    BBL::addPrint(bbl);
    BBL::addMath(bbl);
    BBL::addFileIo(bbl);
    BBL::addOs(bbl);             // new
}
```

### Sandboxing

`addOs` is **not safe** for untrusted code.  It provides:
- Process execution (`execute`, `spawn`, `spawn-detached`)
- Filesystem mutation (`remove`, `rename`, `mkdir`, `chdir`)
- Environment mutation (`setenv`, `unsetenv`)
- Process termination (`exit`)

Embedders who want a sandbox should call `addPrint`, `addMath`, and `addFileIo` individually, omitting `addOs`.  A future `addOsReadOnly` could provide the safe subset (getenv, clock, time, date, getcwd, stat, glob) but is out of scope for v1.

## Considerations

### Why flat namespace, not `os.time()`?

BBL doesn't have module tables or dot-access on tables for function calls.  All stdlib functions live in the global scope (`sin`, `cos`, `print`, `fopen`).  Following established convention, os functions are also global: `getenv`, `time`, `spawn`.  Names are chosen to be unambiguous without a prefix.

### `spawn` vs `io.popen`

Lua uses `io.popen` which returns a file handle.  BBL introduces a distinct **Process** userdata instead, because processes have lifecycle semantics (wait, kill, exit status) that don't map cleanly to the File type.  Process:read and Process:read-line reuse the same interface as File for familiarity.

### `exec` (replace process) — deferred

`exec` (replace the current process image via `execvp`) is a useful POSIX primitive but niche.  Deferred to a future version.  `execute` and `spawn`/`spawn-detached` cover the common shell scripting needs.

### Portability

Implementation targets Linux/POSIX.  Functions use:
- `popen/pclose`, `system`, `execvp`, `fork` — POSIX
- `glob(3)` — POSIX
- `nanosleep` — POSIX
- `setenv/unsetenv` — POSIX
- `stat(2)` — POSIX
- `getpid`, `getcwd`, `chdir`, `mkdir` — POSIX

Windows support is out of scope.  The functions will compile-error or be stubbed on non-POSIX.

### `exit` in embedded mode

`exit` calls `std::exit` which terminates the entire host process.  This is appropriate for the CLI and shell scripts.  Embedders who include `addOs` should be aware that script code can terminate the host.  A future option could make `exit` throw an exception instead, but that is out of scope.

### Error handling

Functions that can fail (remove, rename, mkdir, chdir, spawn) return a boolean or null to indicate failure.  They do not throw BBL errors — the caller checks the return value.  `execute` returns the exit status (0 = success).  `stat` returns null for non-existent paths.

### `glob` returns a table

`glob` returns a table with integer keys (1-based): `(table 1 "/tmp/a" 2 "/tmp/b" ...)`.  This avoids needing string vector support and is consistent with how BBL tables already work as indexed collections.

## Acceptance

1. `(getenv "HOME")` returns a non-null string on Linux.
2. `(setenv "BBL_TEST" "hello")` then `(getenv "BBL_TEST")` returns `"hello"`.
3. `(time)` returns a positive integer.
4. `(clock)` returns a non-negative float.
5. `(sleep 0.01)` pauses execution briefly without error.
6. `(execute "echo hello")` returns 0.
7. `(= proc (spawn "echo hello"))` then `(proc:read)` returns `"hello\n"` and `(proc:wait)` returns 0.
8. `(= pid (spawn-detached "sleep 0"))` returns a positive integer PID.
9. `(date)` returns a date string.
10. `(getcwd)` returns a non-empty string.
11. `(stat "/tmp")` returns a table with `is-dir` = true.
12. `(glob "/tmp/*")` returns a non-empty table with integer keys.
13. `(remove (tmpname))` works for a freshly created temp file.
14. All new functions have tests in `test_bbl.cpp`.
15. `bbl.md` and `api.md` updated with os library documentation.
16. `addOs` is called by `addStdLib`; CLI gets os functions by default.
