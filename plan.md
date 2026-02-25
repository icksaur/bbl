# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items in the plan are complete, remove all items.

## legend

[ ] incomplete
[*] complete

Always follow [style.md](style.md).

---

## phase 1 — lexer, parser, core eval

Deliverable: `bbl.h` and `bbl.cpp` compile.  Unit tests pass for tokenization, parsing, and evaluation of basic expressions.

See `doc/implementation-plan.md` for full phased plan (phases 1–6).

GC note: phase 1 does not include garbage collection.  All managed objects are tracked on an allocation list but freed only by `~BblState`.  Periodic sweep is added in phase 5.

[*] 1. **project skeleton** — create `bbl.h` with public API (`BblState`, `BBL::Error`, `BBL::Type` enum, C function typedef).  Create `bbl.cpp` with stub implementations.  Rename CMake library target to `bbl_lib`.  Add `bbl` CLI executable target (stub `main.cpp`: create `BblState`, load file arg, call `bbl.execfile()`, print errors).  Update test target to link `bbl_lib`.  Verify `cmake -B build && cmake --build build` compiles and `./build/bbl_tests` runs (0 tests).

[*] 2. **lexer** — `class BblLexer` takes `const char*` source, produces tokens via `nextToken()`.  Token types: `LParen`, `RParen`, `Int`, `Float`, `String`, `Symbol`, `Bool`, `Null`, `Dot`, `Binary`, `Eof`.  Skip `//` comments.  Track line numbers.  String escapes: `\"`, `\\`, `\n`, `\t`.  Binary literal: `0b<size>:<N bytes>`.  Unit tests: tokenize `(+ 1 2)`, `(def x "hello")`, `3.14`, `true false null`, comments, string escapes, negative int `-3` vs `(- 3)`, dot `v.x`, binary `0b5:hello`, unterminated string error, binary insufficient bytes error.

[*] 3. **parser** — parse tokens into AST.  Node types: `IntLiteral`, `FloatLiteral`, `StringLiteral`, `BoolLiteral`, `NullLiteral`, `BinaryLiteral`, `Symbol`, `List`, `DotAccess`.  `v.x` → `DotAccess(Symbol(v), "x")`.  `v.x.y` → chained.  `(v.method arg)` → `List` with `DotAccess` head.  Unit tests: parse literals, s-expressions, dot access, nested lists, multiple top-level expressions, unmatched paren errors, empty input.

[*] 4. **BblValue and type system** — tagged union: `BBL::Type` enum (`Null`, `Bool`, `Int`, `Float`, `String`, `Binary`, `Fn`, `Vector`, `Table`, `Struct`, `UserData`).  `BblValue` default-constructs to `Null`.  String → pointer to interned `BblString`.  Binary → pointer to `BblBinary { uint8_t* data; size_t length; }`.  All GC-managed objects go on an allocation list for `~BblState` cleanup.  Unit tests: construct values, verify type tags, value equality.

[*] 5. **string interning** — `BblState` owns intern table (`std::unordered_map`).  `bbl.intern("hello")` returns same pointer on repeat calls.  `~BblState` frees all interned strings.  Unit tests: same string → same pointer, different strings → different pointers.

[*] 6. **scope and variable lookup** — `struct BblScope { std::unordered_map<std::string, BblValue> bindings; BblScope* parent; }`.  `def` adds to current scope.  `set` walks chain (local → parent → root), error if not found.  Unit tests via `bbl.exec()`: `(def x 10)` → getInt = 10, `(def x 10) (set x 20)` → 20, `(set y 5)` → error, `(def x 1) (def x 2)` → 2.

[*] 7. **core eval — arithmetic, comparison, logic** — implement `+` `-` `*` `/` `%` with int/float promotion (int+int=int, float+float=float, int+float=float).  `==` `!=` `<` `>` `<=` `>=`.  Short-circuit `and`/`or` (special forms, bool-only).  `not` (function, bool-only).  `def`, `set`, `if` (then + optional else), `loop` (while).  String concat via `+`.  `if` and `loop` are statements (produce null, not a value).  Unit tests: arithmetic (+,-,*,/,%), promotion, division-by-zero error, comparisons, logic (and/or/not), short-circuit `(or true (set x 1))` → x stays 0, string concat, type errors (non-bool in and/or/not/if, incompatible + types), if/loop as statements → null, if-then, if-else, loop counting, nested expressions.

[*] 8. **functions and closures** — `fn` creates `BblFn` (GC-managed).  Captures free variables as `{name, BblValue}[]` copied from enclosing scope at definition time (value types snapshot, GC types share reference).  Calling fn creates fresh scope with args bound.  Scope lookup: local → captured array.  Last expression is return value.  Unit tests: basic call, multi-arg, zero-arg, arity error, closure value capture (outer rebind doesn't affect captured value), closure write doesn't leak back for value types, higher-order functions, last-expression return.

[*] 9. **`exec` (string eval)** — `bbl.exec("code")` from C++ accumulates into root scope.  `(exec "code")` from script creates fresh isolated scope, returns last expression.  Unit tests: exec defines variable readable from C++, two execs accumulate, script-level exec returns value and isolates scope.

[*] 10. **validate phase 1** — `cmake -B build && cmake --build build` succeeds.  `./build/bbl_tests` — all phase 1 unit tests pass.