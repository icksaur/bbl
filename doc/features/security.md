# Security & Sandboxing

## goal

A BBL script cannot access files outside its own directory tree unless the C++ host explicitly opts in.  Without any standard library loaded, a script has **zero** filesystem access — it can only compute with the values the host provides.

## sandbox model

BBL's security is **opt-in by capability**.  The C++ host controls what a script can do:

| capability | how it's granted | access |
|------------|------------------|--------|
| no stdlib loaded | default | no filesystem access at all — pure computation |
| `BBL::addFileIo(bbl)` | C++ call | `fopen`, `filebytes` — filesystem access with sandbox rules |
| `BBL::addPrint(bbl)` | C++ call | stdout only — no filesystem |
| `BBL::addMath(bbl)` | C++ call | math functions — no filesystem |
| `bbl.defn(...)` | C++ call | whatever the host function exposes |

The special form `execfile` is always available (it is a language primitive, not a library function).  It is subject to **path sandboxing** described below.

## path sandboxing

Both `execfile` and `filebytes` enforce the same path rules:

1. **Relative paths only.**  Absolute paths (starting with `/` or a drive letter) are a runtime error.
2. **No parent traversal.**  Any path containing `..` is a runtime error.
3. **Resolved relative to the calling script's directory.**  If `game/main.bbl` calls `(filebytes "tex.png")`, it resolves to `game/tex.png`.

These rules mean a script can only reach files in its own directory or child directories — never above, never absolute.

### chaining rule for `execfile`

`execfile` **chains the sandbox down**.  The exec'd script's sandbox root is the directory of the file being executed, not the original caller.

```
project/
├── main.bbl
└── subdir/
    ├── helper.bbl
    └── data/
        └── config.bbl
```

```bbl
// main.bbl — sandbox root is project/
(execfile "subdir/helper.bbl")    // OK: child directory
```

```bbl
// subdir/helper.bbl — sandbox root is now project/subdir/
(execfile "data/config.bbl")      // OK: child of subdir/
(execfile "../main.bbl")          // ERROR: .. not allowed
(filebytes "data/sprite.png")     // OK: child of subdir/
(filebytes "../texture.png")      // ERROR: .. not allowed
```

Each `execfile` narrows the sandbox.  A script in `subdir/` cannot escape back to `project/`.

### chaining rule for `exec`

`exec` evaluates a string as code.  The resulting code inherits the **same sandbox root** as the calling script — it does not narrow further since there is no new file path.

```bbl
// main.bbl — sandbox root is project/
(exec "(filebytes \"tex.png\")")  // OK: resolves to project/tex.png
```

## C++ API path resolution

`bbl.execfile()` and C++ calls to `filebytes` use different resolution:

- **`bbl.execfile("file.bbl")`** — resolves relative to the process's current working directory (or a configurable base path on `BblState`).  No sandbox restriction — the C++ host is trusted.
- **`bbl.exec("code string")`** — evaluates the string.  Any `filebytes` or `execfile` calls within it resolve relative to CWD / configured base path.  No sandbox restriction.

The sandbox rules apply only to script-level `execfile` and `filebytes` — not to C++ API calls.  The C++ host has full filesystem access by definition.

## `fopen` — no sandboxing

`fopen` (from `BBL::addFileIo`) is **not sandboxed**.  It follows standard C `fopen()` semantics — any path the OS allows.  This is intentional:

- `fopen` is opt-in.  If the host doesn't call `BBL::addFileIo(bbl)`, scripts have no `fopen`.
- Use cases that need `fopen` (build tools, data pipelines) need unrestricted access.
- For untrusted scripts, simply don't load file I/O.

## summary

| operation | sandbox | access |
|-----------|---------|--------|
| `execfile` (from script) | yes | script dir + children, chains down |
| `filebytes` (from script) | yes | script dir + children |
| `exec` (from script) | inherits | same sandbox as caller |
| `fopen` (from `addFileIo`) | no | unrestricted (opt-in) |
| `bbl.execfile()` (C++ API) | no | host is trusted |
| `bbl.exec()` (C++ API) | no | host is trusted |

## open questions

None.
