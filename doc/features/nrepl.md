# nREPL — Interactive Evaluation in Running BBL Programs

Status: proposed

## Goal

Enable live evaluation of BBL forms inside a running user program, with
module-aware context targeting. The user starts their program, the editor
connects, and eval runs in the user's process with all their state.

This follows the Clojure nREPL / Common Lisp SLIME model: the user owns
the process, the editor is just a client.

## Background

### How Lisp ecosystems solve this

**Clojure (nREPL):**
- User starts JVM with nREPL server embedded (`lein repl`, port file)
- Editor connects to TCP port
- Eval executes in the JVM with full application state
- Results sent back as structured data (value, stdout, stderr)

**Common Lisp (Swank/SLIME):**
- User starts SBCL with Swank server loaded
- Emacs connects to TCP port
- Can redefine any function in the running image
- All callers see new definition immediately (same object, new code)

**Key principle:** The eval server is embedded IN the user's process.
It's not a separate tool — it's part of the running application.

### What BBL has today

**Module environments (implemented):**
- `(import "physics.bbl")` creates a `BblTable` env for the module
- `moduleCache` maps canonical paths → env tables
- `currentEnv` controls where OP_ENVSET writes (env table vs globals)
- Closures capture their env → imported functions see their module's bindings
- Redefining a function in the env table is visible to all callers immediately

**Channels (implemented):**
- `(channel)` creates a first-class message queue
- `send`, `recv`, `try-recv`, `close`
- Can pass between main thread and child states

**TCP sockets (implemented):**
- `tcp-listen`, `tcp-connect`, socket read/write
- Already used for HTTP and networking

## Design

### Architecture

```
┌─────────────────────────────────────────────┐
│  User's BBL Process                         │
│                                             │
│  main.bbl                                   │
│    ├── (import "physics.bbl")  → env table  │
│    ├── (import "render.bbl")   → env table  │
│    └── (nrepl 7888)            → blocks     │
│         └── processes eval requests via      │
│             select on child's message queue  │
│                                             │
│  nREPL child state (TCP server)             │
│    ├── accepts editor connections            │
│    ├── reads JSON-RPC requests              │
│    └── posts eval requests to parent        │
└────────────────┬────────────────────────────┘
                 │ TCP :7888
┌────────────────┴────────────────────────────┐
│  Editor (VS Code / Emacs / vim)             │
│    └── sends eval request for selected form │
└─────────────────────────────────────────────┘
```

### API: Blocking vs Non-Blocking

**`(nrepl port)`** — Blocking. Starts the TCP eval server and enters a
select loop processing eval requests until the process is killed. The
script does not continue past this call. Use for setup scripts and
interactive development:

```bbl
(import "physics.bbl")
(import "render.bbl")
(nrepl 7888)   // blocks here — process stays alive, accepting evals
```

**`(nrepl-start port)`** — Non-blocking. Returns a handle. The caller
must poll via `(nrepl-poll handle)` in their own loop. Use for game
loops and servers that need to interleave eval with frame updates:

```bbl
(import "physics.bbl")
(= repl (nrepl-start 7888))
(loop running
  (nrepl-poll repl)        // drain + execute pending evals
  (physics:step dt)
  (render:frame))
```

**`(nrepl-poll handle)`** — Non-blocking. Checks for pending eval
requests via `try-recv` on the nREPL child. If a request is pending,
executes it on the main thread (module-targeted if file specified),
posts the result back to the child.

### Process Ownership

The user starts and stops their program. `(nrepl 7888)` or
`(nrepl-start 7888)` writes `.bbl-nrepl-port` in the working directory.
On shutdown, the file is deleted. No orphan processes.

Child states can also start their own nREPL on a different port:
```bbl
// worker.bbl (runs as child state)
(import "ai.bbl")
(nrepl 7889)   // own nREPL, evals in worker's module envs
```

Multiple port files: `.bbl-nrepl-port` contains JSON mapping names to
ports, or each process writes a unique file.

### Thread Safety

BBL is single-threaded. The nREPL TCP server runs in a child state (separate
thread), but eval MUST run on the main thread because it accesses the parent's
BblState (globals, module envs, GC heap).

Solution: the nREPL child posts eval requests to a channel. The parent's
main loop calls `(nrepl-poll repl)` which:
1. Calls `try-recv` on the eval channel
2. If a request is pending, executes it on the main thread
3. Posts the result back via a response channel
4. The nREPL child sends the response to the TCP client

This is exactly how Clojure nREPL handles thread safety — eval requests
are queued and executed on the appropriate thread.

### Protocol

JSON-RPC 2.0 over TCP. Messages use `Content-Length` framing (same as LSP).

**Requests:**

```json
{"jsonrpc": "2.0", "id": 1, "method": "eval",
 "params": {"code": "(= gravity -20)", "file": "physics.bbl"}}
```

`file` is optional. If provided, eval runs in that module's env.
If omitted, eval runs in the root scope (globals).

```json
{"jsonrpc": "2.0", "id": 2, "method": "eval",
 "params": {"code": "(player:position)"}}
```

**Responses:**

```json
{"jsonrpc": "2.0", "id": 1, "result": {"value": "-20", "type": "int"}}
```

```json
{"jsonrpc": "2.0", "id": 1, "result": {"value": null, "output": "hello\n"}}
```

Output from `print` during eval is captured via `printCapture` and returned
in the `output` field, separate from the result value.

**Error response:**

```json
{"jsonrpc": "2.0", "id": 1, "error": {"message": "undefined variable: foo", "line": 3}}
```

**Methods:**

| Method | Params | Description |
|--------|--------|-------------|
| `eval` | `code`, `file?` | Evaluate code in module context |
| `completions` | `prefix`, `file?` | List matching symbols |
| `modules` | — | List loaded modules (moduleCache keys) |
| `interrupt` | — | Cancel running eval (future) |

### Module Targeting

When `file` is provided in an eval request:

1. Canonicalize the path (same as `import` does)
2. Look up in `moduleCache`
3. If found: set `currentEnv = env`, execute code, restore `currentEnv`
4. If not found: return error "module not loaded"

The definition goes into the module's env table. Because imported closures
hold references to the same env table, all callers see the change immediately.
No restart needed.

```
Editor sends: {"method": "eval", "params": {"code": "(= step (fn (dt) ...))", "file": "physics.bbl"}}

What happens:
1. Look up "physics.bbl" in moduleCache → BblTable* env
2. currentEnv = env
3. exec("(= step (fn (dt) ...))")
4. OP_ENVSET writes new "step" closure into env table
5. currentEnv = nullptr
6. Any code that called (physics:step) now calls the new function
```

### Editor Discovery

`(nrepl 7888)` writes a file `.bbl-nrepl-port` containing just the port
number. The editor extension reads this file to find the running process.
On shutdown, the file is deleted.

This is the same convention Clojure/Leiningen uses (`.nrepl-port`).

### Editor Integration (VS Code)

The bbl-vscode extension adds:
- **Eval selection** (Ctrl+Enter): Send selected form to nREPL
- **Eval file** (Ctrl+Shift+Enter): Send entire file contents
- **Module indicator**: Status bar shows connected module
- **Output panel**: Shows captured print output from eval
- **Auto-connect**: Reads `.bbl-nrepl-port` on workspace open

## Implementation

### Phase 1: nrepl-exec C function (~30 LOC)

Register `nrepl-exec` in addStdLib. Takes code string + optional file path.
Switches `currentEnv`, captures print output, returns result table:

```cpp
static int bblNreplExec(BblState* bbl) {
    const char* code = bbl->getStringArg(0);
    const char* file = bbl->argCount() > 1 ? bbl->getStringArg(1) : nullptr;

    BblTable* savedEnv = bbl->currentEnv;
    if (file && strlen(file) > 0) {
        auto key = std::filesystem::weakly_canonical(file).string();
        auto it = bbl->moduleCache.find(key);
        if (it != bbl->moduleCache.end())
            bbl->currentEnv = it->second;
        else
            throw BBL::Error{"nrepl-exec: module not loaded: " + std::string(file)};
    }

    std::string output;
    bbl->printCapture = &output;
    try {
        BblValue result = bbl->execExpr(code);
        bbl->currentEnv = savedEnv;
        bbl->printCapture = nullptr;
        BblTable* tbl = bbl->allocTable();
        tbl->set(BblValue::makeString(bbl->intern("value")),
                 BblValue::makeString(bbl->allocString(valueToString(result))));
        if (!output.empty())
            tbl->set(BblValue::makeString(bbl->intern("output")),
                     BblValue::makeString(bbl->allocString(std::move(output))));
        bbl->pushTable(tbl);
    } catch (const BBL::Error& e) {
        bbl->currentEnv = savedEnv;
        bbl->printCapture = nullptr;
        BblTable* tbl = bbl->allocTable();
        tbl->set(BblValue::makeString(bbl->intern("error")),
                 BblValue::makeString(bbl->allocString(e.what)));
        bbl->pushTable(tbl);
    }
    return 1;
}
```

### Phase 2: nrepl-server.bbl (~60 LOC)

Shipped as a stdlib script. The child state runs this:

```bbl
(= config (recv))
(= server (tcp-listen config.port))
(os:write-file ".bbl-nrepl-port" (str config.port))

(loop true
  (= conn (server:accept))
  (= header (conn:read-line))
  (= blank (conn:read-line))
  (= len (int (str:slice header 16)))
  (= body (conn:read-bytes len))
  (= req (json:decode body))

  (if (== req.method "eval")
    (do
      (post (table "id" req.id "code" req.params.code
                   "file" (if req.params.file req.params.file "")))
      (= resp (recv))
      (= json-resp (json:encode (table "jsonrpc" "2.0" "id" resp.id "result" resp)))
      (= msg (str "Content-Length: " (json-resp:length) "\r\n\r\n" json-resp))
      (conn:write msg)))

  (conn:close))
```

### Phase 3: Blocking / non-blocking API (~40 LOC)

Implemented as C functions in addStdLib:

**`(nrepl port)`** — blocking:
1. Spawn child state running nrepl-server.bbl
2. Post port config to child
3. Enter select loop: `(loop true (nrepl-poll child) ...)`
4. Uses `select` to block until child posts (zero CPU idle)

**`(nrepl-start port)`** — non-blocking:
1. Same spawn + config as above
2. Returns child handle immediately

**`(nrepl-poll handle)`** — non-blocking drain:
1. `(= req (handle:try-recv))`
2. If req: `(= result (nrepl-exec req.code req.file))`
3. `(handle:post (table "id" req.id ...))`

### Phase 4: Tests

Unit tests (tests/test_bbl.cpp):
- `test_nrepl_exec_root`: eval in root scope returns correct result
- `test_nrepl_exec_module`: eval in module scope writes to env table
- `test_nrepl_exec_redefine`: redefine function visible to callers
- `test_nrepl_exec_print_capture`: print output captured in result
- `test_nrepl_exec_error`: error returned as structured table
- `test_nrepl_exec_unknown_module`: error for module not in cache

Functional test:
- `test_nrepl_tcp_roundtrip.bbl`: start nrepl-start, connect via TCP,
  send eval request, verify response

### Phase 5: VS Code Extension

Add to bbl-vscode:
- `bbl.evalSelection` command (Ctrl+Enter): send selection to nREPL
- `bbl.evalFile` command (Ctrl+Shift+Enter): send entire file
- Status bar: connection indicator + module name
- Output panel: shows captured print output
- Auto-connect: read `.bbl-nrepl-port` on workspace open
- Disconnect on file deletion (process died)

## Port File Convention

| File | Content | Created by | Deleted by |
|------|---------|------------|------------|
| `.bbl-nrepl-port` | `7888` | `(nrepl 7888)` | Process exit (or explicit cleanup) |

Editor scans workspace root for this file on activation and auto-connects.

## Acceptance Criteria

1. `(nrepl 7888)` starts TCP eval server, writes port file
2. `(nrepl-poll child)` drains eval requests and executes on main thread
3. Eval in root scope works (no file param)
4. Eval in module scope works (file param → moduleCache lookup → currentEnv)
5. Redefined functions are visible to all callers immediately
6. Print output captured and returned separately from result
7. Errors returned as structured error objects
8. Port file deleted on clean shutdown
9. Multiple editor connections supported (one at a time)
10. No interference with program execution between polls

## Risks

**Main thread starvation:** If eval takes too long, it blocks the main loop.
Mitigation: maxSteps limit on eval, timeout, or `interrupt` method (future).

**GC during eval:** Eval may allocate and trigger GC. This is fine — the
main thread owns the BblState and GC is safe here.

**Module not loaded:** If the editor requests eval in a module that hasn't
been imported yet, return an error. Don't auto-import — the user's process
controls what's loaded.

**Port conflicts:** If port is in use, `tcp-listen` throws. The user picks
a different port or kills the old process. No magic.

**Security:** The nREPL server listens on localhost only. No authentication.
This is development tooling, not production infrastructure. Same model as
Clojure nREPL.
