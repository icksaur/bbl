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
│    ├── (nrepl 7888)            → child state│
│    └── main loop                            │
│         ├── (nrepl-poll)   ←── eval channel │
│         ├── (physics:step)                  │
│         └── (render:frame)                  │
│                                             │
│  nREPL child state (TCP server)             │
│    ├── accepts editor connections            │
│    ├── reads JSON-RPC requests              │
│    └── posts eval requests → eval channel   │
└────────────────┬────────────────────────────┘
                 │ TCP :7888
┌────────────────┴────────────────────────────┐
│  Editor (VS Code / Emacs / vim)             │
│    └── sends eval request for selected form │
└─────────────────────────────────────────────┘
```

### Process Ownership

The user starts their program. The program opts into nREPL:

```bbl
(import "physics.bbl")
(import "render.bbl")

(= repl (nrepl 7888))

(loop running
  (nrepl-poll repl)       // drain eval requests, execute on main thread
  (physics:step dt)
  (render:frame))
```

`(nrepl 7888)` spawns a child state that:
1. Listens on TCP port 7888
2. Writes port to `.bbl-nrepl-port` in current directory (editor discovery)
3. Accepts connections
4. Reads JSON-RPC messages
5. Posts eval requests to a channel shared with the parent
6. The parent drains the channel via `(nrepl-poll repl)`

The user stops the program normally. The nREPL server dies with it.
No orphan processes. No lifecycle management.

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

### BBL stdlib additions (~100 LOC)

```bbl
// nrepl.bbl — shipped as stdlib module
// Called via (import "nrepl") or (nrepl port)

(= nrepl (fn (port)
  (= eval-ch (channel))
  (= resp-ch (channel))
  (= child (state-new "nrepl-server.bbl"))
  (child:post (table "port" port "eval-ch-id" 0))
  (table "child" child "eval-ch" eval-ch "resp-ch" resp-ch)))

(= nrepl-poll (fn (repl)
  (= req (repl.eval-ch:try-recv))
  (if req (do
    // Execute in module context
    (= result (nrepl-exec req.code req.file))
    (repl.resp-ch:send result)))))
```

Actually, the channel can't be passed between states (it's a userdata
in the parent's GC heap). The real implementation needs a different approach:
the nREPL child reads TCP, posts a BblMessage (table with code+file fields)
to the parent via the existing state post/recv mechanism. The parent polls
via `try-recv` on the child handle.

### Revised architecture using existing post/recv:

```bbl
(= nrepl (fn (port)
  (= child (state-new "nrepl-server.bbl"))
  (child:post (table "port" port))
  child))

(= nrepl-poll (fn (child)
  (= req (child:try-recv))
  (if req (do
    (= result (nrepl-exec req.code req.file))
    (child:post (table "id" req.id "value" result))))))
```

The nREPL child:
1. Receives port config via `(recv)`
2. Starts TCP listener
3. On request: `(post (table "id" id "code" code "file" file))`
4. Waits for response: `(recv)` → sends back to TCP client

### C++ additions (~30 LOC)

`nrepl-exec` needs to be a C function that:
1. Takes code string + optional file path
2. Looks up module in parent's `moduleCache`
3. Sets `currentEnv`
4. Calls `execExpr(code)`
5. Restores `currentEnv`
6. Returns result as string

```cpp
static int bblNreplExec(BblState* bbl) {
    const char* code = bbl->getStringArg(0);
    const char* file = bbl->argCount() > 1 ? bbl->getStringArg(1) : nullptr;

    BblTable* savedEnv = bbl->currentEnv;
    if (file) {
        auto key = std::filesystem::weakly_canonical(file).string();
        auto it = bbl->moduleCache.find(key);
        if (it != bbl->moduleCache.end())
            bbl->currentEnv = it->second;
    }

    std::string output;
    bbl->printCapture = &output;
    try {
        BblValue result = bbl->execExpr(code);
        bbl->currentEnv = savedEnv;
        bbl->printCapture = nullptr;
        // Return result as table with value + output
        BblTable* tbl = bbl->allocTable();
        tbl->set(BblValue::makeString(bbl->intern("value")),
                 BblValue::makeString(bbl->allocString(valueToString(result))));
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

Register as `bbl.defn("nrepl-exec", bblNreplExec)` in addStdLib.

### nrepl-server.bbl (~60 lines)

Shipped as a stdlib script. The child state runs this:

```bbl
(= config (recv))
(= server (tcp-listen config.port))

// Write port file for editor discovery
(os:write-file ".bbl-nrepl-port" (str config.port))

(loop true
  (= conn (server:accept))
  // Read Content-Length header + JSON body
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

  (if (== req.method "modules")
    (do
      // Return list of loaded module paths
      // ... 
      ))

  (conn:close))
```

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
