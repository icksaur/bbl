# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

## Phase 1: LSP MVP — Syntax Highlighting + Diagnostics

### VS Code extension
- [ ] Create `editor/vscode/bbl-language/` with:
  - `package.json`: register `.bbl` language, TextMate grammar, LSP client (spawns `bbl --lsp` via stdio).
  - `syntaxes/bbl.tmLanguage.json`: TextMate grammar. Comments use `//` (not `;`). Strings use `([^"\\\\]|\\\\.)*` for escape handling. Binary prefixes `0b[0-9]+:` and `0z[0-9]+:`. Keywords matched after `(` or whitespace. Method calls with `:` prefix.
  - `language-configuration.json`: auto-close parens, bracket pairs `(/)`, comment toggling `//`, auto-indent, surround pairs.
  - `src/extension.js`: minimal LSP client using `vscode-languageclient`, start `bbl --lsp`, stdio transport. No TypeScript build step.
- [ ] `npm init` + add `vscode-languageclient` dependency.

### JSON library
- [ ] Vendor yyjson (~15KB C) into `vendor/yyjson/`. Add to CMakeLists.txt as static lib.

### LSP server (`src/lsp.cpp`)
- [ ] **JSON-RPC transport:** Read `Content-Length: N\r\n\r\n` + N bytes from stdin. Parse with yyjson. Write responses with same framing to stdout. No stderr output.
- [ ] **Initialize:** Respond with capabilities: `textDocumentSync: Full`, `completionProvider: {}`, `hoverProvider: true`. Handle `initialized` notification.
- [ ] **Document sync:** `didOpen` stores text, `didChange` updates (full sync), `didClose` removes.
- [ ] **Diagnostics:** On open/change, lex+parse with `BblLexer` + `parse()`. Catch `BBL::Error`, map line to LSP range. Publish via `textDocument/publishDiagnostics`. Clear diagnostics on successful parse.
- [ ] **Shutdown/exit:** Return null on `shutdown`, exit(0) on `exit`.

### CLI
- [ ] In `main.cpp`, add `--lsp` flag: call `lspMain()` from `src/lsp.cpp`.
- [ ] Add `src/lsp.cpp` to CMakeLists.txt (part of bbl executable target).

### Test
- [ ] Pipe a JSON-RPC initialize message to `echo '...' | bbl --lsp` and verify valid response.
- [ ] Install extension locally: symlink to `~/.vscode/extensions/bbl-language` or use `npx vsce package && code --install-extension bbl-language-0.0.1.vsix`.
- [ ] Verify: open a `.bbl` file, see syntax highlighting, introduce a parse error, see red squiggle.

## Phase 2: Completions + Folding

- [ ] `textDocument/completion`: context-aware.
  - After `(`: keywords (`if`, `loop`, `fn`, `each`, `do`, `with`, `try`, `break`, `continue`, `and`, `or`, `not`) + type constructors (`vector`, `table`, `struct`, `binary`, `int`, `sizeof`) + builtins (`print`, `str`, `typeof`, `float`, `fmt`, math, file I/O, OS functions).
  - After `:`: method names. Filter by receiver type if inferrable from preceding `(= var (table/vector/...))` binding.
  - After `(struct Name`: type names (`int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32`, `float64`, `bool`, plus registered struct names).
  - After `(= `: suppress completions (binding target).
  - In `//` comment: suppress completions.
  - Bare identifier: in-scope variables (walk AST for `(= name ...)` above cursor) + builtins.
- [ ] `textDocument/foldingRange`: return ranges for each top-level s-expression (matched parens). Nested folding for deep expressions.
- [ ] `textDocument/selectionRange`: for cursor position, return nested ranges from innermost paren pair outward.

## Phase 3: Hover + Go-to-Definition

- [ ] `textDocument/hover`: static table mapping builtin names → signature + description. Variables → "defined at line N". Methods → "available on: Table, String, ..." Keywords → syntax description.
- [ ] `textDocument/definition`: walk AST for `(= name ...)` matching cursor symbol. Return Location.
- [ ] `textDocument/references`: find all occurrences of symbol in document.
- [ ] `textDocument/signatureHelp`: for `(fn-name arg1 arg2 |)`, show function arity and parameter names if defined via `(= name (fn (params) ...))`.
