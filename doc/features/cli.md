# The `bbl` Command-Line Tool

## overview

The `bbl` binary is the standalone interpreter.  Like Lua's `lua` command, it runs scripts from files or starts an interactive REPL.  It creates a `BblState`, loads the standard library, and executes.

## usage

```
bbl [options] [script [args...]]
```

With a script argument, `bbl` executes the file and exits.  With no arguments, it enters interactive mode.

## running a script

```sh
bbl script.bbl
```

The interpreter:

1. Creates a `BblState`.
2. Calls `BBL::addStdLib(bbl)` — registers `print`, file I/O, math, string methods.
3. Calls `bbl.execfile("script.bbl")`.
4. On success, exits with code `0`.
5. On error, prints the error and backtrace to stderr, exits with code `1`.

The script runs in the root scope.  Any variables it defines are discarded on exit.

### script arguments

Arguments after the script name are available to the script as a table bound to `args`:

```sh
bbl build.bbl output.obj --verbose
```

```bbl
// args is a table: (table 1 "output.obj" 2 "--verbose")
(= i 0)
(loop (< i (args.length))
    (print (args.at i) "\n")
    (= i (+ i 1))
)
```

`args` is injected into the root scope before the script executes.  If no arguments are passed, `args` is an empty table.

## interactive mode (REPL)

```sh
bbl
```

With no arguments, `bbl` starts a read-eval-print loop.  The environment persists across inputs — definitions accumulate, just like `bbl.execfile()` / `bbl.exec()` from C++.

```
> (print "hello")
hello
> (= greet (fn (name)
.     (print "hi " name "\n")
. ))
> (greet "world")
hi world
```

### REPL behavior

- **Prompt**: `> ` for a new expression.  `. ` while inside an incomplete expression (unmatched parentheses).
- **Multi-line input**: the REPL waits for balanced parentheses before evaluating.  An open `(` without a matching `)` continues to the next line.
- **Expression result**: if the last evaluated expression produces a value, it is printed.  `null` and void results are silent.
- **Errors**: printed to stderr with a backtrace.  The REPL continues — previous definitions are still alive.
- **Exit**: `Ctrl-D` (EOF) or `Ctrl-C` exits.

### REPL standard library

The REPL loads `addStdLib` automatically.  `print`, `fopen`, `filebytes`, math functions, and all standard types are available from the first prompt.

## options

| option | description |
|--------|-------------|
| `-e "expr"` | evaluate a string and exit.  `bbl -e '(print "hello")'` |
| `-v` / `--version` | print version string and exit |
| `-h` / `--help` | print usage and exit |

`-e` creates a `BblState` with the standard library, evaluates the expression, and exits.  Useful for one-liners and shell scripting.

```sh
bbl -e '(print (+ 1 2))'
3
```

Multiple `-e` flags are evaluated in order within the same environment:

```sh
bbl -e '(= x 10)' -e '(print (* x x))'
100
```

## exit codes

| code | meaning |
|------|---------|
| `0` | success |
| `1` | runtime error (type mismatch, undefined symbol, etc.) |
| `2` | file not found or I/O error |

## environment variables

| variable | description |
|----------|-------------|
| `BBL_PATH` | colon-separated list of directories to search when `execfile` resolves relative paths.  If unset, `execfile` resolves relative to the calling script's directory (or CWD for the top-level script). |

## comparison with Lua

| feature | `lua` | `bbl` |
|---------|-------|-------|
| run script | `lua script.lua` | `bbl script.bbl` |
| interactive | `lua` | `bbl` |
| eval string | `lua -e 'print(1)'` | `bbl -e '(print 1)'` |
| script args | `arg` table (0-indexed, negative for flags) | `args` table (0-based via `at`) |
| version | `lua -v` | `bbl -v` |
| stdlib | `luaL_openlibs` | `BBL::addStdLib` |

## open questions

None.
