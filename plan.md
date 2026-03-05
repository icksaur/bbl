# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

## VS Code extension for BBL (../bbl-vscode/)

### Scaffold
- [ ] Create `../bbl-vscode/` with `package.json`, `language-configuration.json`, `syntaxes/bbl.tmLanguage.json`, `src/extension.js`.
- [ ] `npm init` and add `vscode-languageclient` as dependency.

### package.json
- [ ] Name: `bbl-language`. Publisher: `icksaur`. Engines: `vscode ^1.75.0`.
- [ ] Register language `bbl` with extensions `.bbl`.
- [ ] Register TextMate grammar `source.bbl`.
- [ ] Register language configuration.
- [ ] Main: `src/extension.js`. Activation: `onLanguage:bbl`.

### TextMate grammar (syntaxes/bbl.tmLanguage.json)
- [ ] Comments: `//` line comments, `#!` shebang.
- [ ] Strings: begin/end `"` with escape captures for `\"`, `\\`, `\n`, `\t`.
- [ ] Numbers: integers and floats.
- [ ] Binary literals: `0b[0-9]+:` and `0z[0-9]+:` prefixes.
- [ ] Keywords: `if`, `loop`, `each`, `fn`, `do`, `with`, `try`, `catch`, `break`, `continue`, `and`, `or`, `not`.
- [ ] Type constructors: `vector`, `table`, `struct`, `binary`, `int`, `sizeof`, `exec`, `execfile`.
- [ ] Constants: `true`, `false`, `null`.
- [ ] Builtins: `print`, `str`, `typeof`, `float`, `fmt`, `compress`, `decompress`, math functions.
- [ ] Method calls: `:method-name` pattern.
- [ ] Variable bindings: `(= name` captures name as variable.

### language-configuration.json
- [ ] Comments: lineComment `//`.
- [ ] Brackets: `(` `)`.
- [ ] Auto-closing pairs: parens, double quotes.
- [ ] Folding markers: `(` and `)`.

### extension.js (LSP client)
- [ ] Import `vscode-languageclient`.
- [ ] On activate: create LanguageClient with server command `bbl --lsp`, stdio transport.
- [ ] On deactivate: stop client.

### Install bbl (prerequisite)
- [ ] Update PKGBUILD in bbl/ if needed. `makepkg -si` installs `bbl` to /usr/bin.
- [ ] Verify `bbl --lsp` works from PATH.

### Install extension
- [ ] `cd ../bbl-vscode && npm install && npx vsce package` → produces .vsix.
- [ ] `code --install-extension bbl-language-0.0.1.vsix`.
- [ ] Or: symlink `ln -s $(pwd) ~/.vscode/extensions/bbl-language`.

### Test
- [ ] Open a `.bbl` file in VS Code. Verify syntax highlighting.
- [ ] Introduce parse error. Verify red squiggle.
- [ ] Type `(` and verify completion list appears.
- [ ] Hover over `print` and verify signature popup.
