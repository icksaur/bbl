# plan: compressed binary blobs

Per `doc/features/compressed-binary.md`. Depends on LZ4 (`-llz4`).

## steps

- [ ] **CMake: add LZ4** — `find_library(LZ4 lz4)`, link to bbl target. Optional via `HAVE_LZ4` define.
- [ ] **Lexer: `0z` prefix** — In number parsing, detect `0z` like `0b`. Same readBinary logic but sets `isCompressed=true` on token. Add `bool isCompressed` to Token and AstNode.
- [ ] **BblBinary: compressed materialize** — Add `bool compressed` field. `materialize()` calls `LZ4F_decompress` when compressed. `length()` reads LZ4 frame header for content size via `LZ4F_getFrameInfo()` without full decompression.
- [ ] **Compiler: propagate compressed flag** — BinaryLiteral with `isCompressed` creates lazy binary with `compressed=true`.
- [ ] **CLI: `bbl --compress`** — In main.cpp, new flag. Lex input file, find `0b` tokens, compress payloads with `LZ4F_compressFrame()`, output source with `0b` → `0z` and compressed payloads. Non-binary tokens output verbatim.
- [ ] **C++ API** — `BBL::compress(data, size)` and `BBL::decompress(data, size)` returning `vector<uint8_t>`. Pure LZ4 wrappers.
- [ ] **Stdlib** — Register `compress` and `decompress` as BBL functions taking and returning BblBinary.
- [ ] **Tests** — Compress a binary literal, verify decompression matches. Test `:length` without decompressing. Round-trip test.
