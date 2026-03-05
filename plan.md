# plan: lazy binary loading

Per `doc/features/lazy-binary.md`.

## steps

- [ ] **Lexer: skip binary data** — In `readBinary()` (bbl.cpp ~line 247), instead of copying bytes into `Token::binaryData`, store `binarySource` pointer (into source string) and `binarySize`. Skip forward by `size` bytes but still scan for `\n` to maintain line counter accuracy. Add `const char* binarySource` and `size_t binarySize` fields to Token struct.
- [ ] **AST: store reference** — Change `AstNode::binaryData` from `vector<uint8_t>` to `const char* binarySource + size_t binarySize`. Update `parsePrimary()` to copy pointer/size, not data.
- [ ] **BblBinary: add lazy fields** — Add `const char* lazySource = nullptr` and `size_t lazySize = 0` to BblBinary. Add `materialize()` method. Update `length()` to return `lazySize` when unmaterialized. Add `allocLazyBinary(const char* src, size_t size)` factory on BblState.
- [ ] **Compiler: create lazy constant** — In compiler.cpp BinaryLiteral handling, create BblBinary via `allocLazyBinary()` instead of copying data via `allocBinary()`.
- [ ] **JIT: materialize on access** — In jitMcall binary methods (`:at`, `:set`, `:slice`, `:copy-from`, `:resize`), call `bin->materialize()` before accessing `data`. In `jitBinary()` for binary→vector conversion, materialize. `:length` uses `length()` which handles lazy case without materializing.
- [ ] **C++ API: materialize on access** — In `getBinary()` and `getBinaryArg()`, call `materialize()` before returning pointer to host code. In `File.write-bytes`, materialize before writing.
- [ ] **End-of-exec sweep** — At end of `exec()` and `execExpr()`, walk `gcHead` unconditionally and call `materialize()` on all lazy BblBinary objects. This prevents dangling source pointers in REPL mode and after execfile().
- [ ] **Tests** — Verify all existing binary tests pass unchanged. Add test: large binary literal where only `:length` is queried (should not allocate data vector).
