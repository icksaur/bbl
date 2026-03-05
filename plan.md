# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

## LSP server: `bbl --lsp`

### Vendor yyjson
- [ ] Download yyjson.c + yyjson.h into `vendor/yyjson/`. Add to CMakeLists.txt as static lib.

### LSP core (`src/lsp.cpp`)
- [ ] JSON-RPC transport: read `Content-Length: N\r\n\r\n<N bytes>` from stdin, parse with yyjson. Write responses with same framing to stdout.
- [ ] Initialize handshake: respond with capabilities (textDocumentSync=Full, completionProvider, hoverProvider, definitionProvider, foldingRangeProvider). Handle `initialized`.
- [ ] Document sync: `didOpen` stores (uri → text), `didChange` updates, `didClose` removes.
- [ ] Diagnostics: on open/change, lex+parse. Catch BBL::Error, publish diagnostics with line/character range.
- [ ] Completions: after `(` → keywords + builtins. After `:` → method names. Bare symbol → in-scope variables + builtins.
- [ ] Hover: static table of builtin signatures. Variables → definition line.
- [ ] Go-to-definition: find `(= name ...)` binding for symbol under cursor.
- [ ] Folding ranges: matched paren pairs as foldable regions.
- [ ] Shutdown/exit.

### CLI integration
- [ ] `main.cpp`: `--lsp` flag calls `lspMain()`.
- [ ] `src/lsp.cpp` added to CMakeLists.txt.

### Tests
- [ ] Unit test in `test_bbl.cpp`: construct JSON-RPC initialize message, pipe to lspMain via string, verify response contains capabilities.
- [ ] Unit test: send didOpen with parse error, verify diagnostics notification.
- [ ] Shell script `tests/test_lsp.sh`: pipe multi-message JSON-RPC sequence to `bbl --lsp`, validate responses.
- [ ] Verify all 736 existing tests + 24 functional tests still pass.

## VS Code extension (deferred — separate repo)
