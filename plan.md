# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

## CMake: vendor LZ4 static library

- [ ] In `CMakeLists.txt`, add after the `bbl_lib` target:
  ```cmake
  add_library(lz4 STATIC vendor/lz4/lz4.c vendor/lz4/lz4frame.c vendor/lz4/lz4hc.c vendor/lz4/xxhash.c)
  set_target_properties(lz4 PROPERTIES C_STANDARD 99)
  target_include_directories(lz4 PUBLIC vendor/lz4)
  target_link_libraries(bbl_lib PUBLIC lz4)
  ```
- [ ] Verify build: `cmake --build build -j$(nproc)` compiles LZ4 and links.

## Lexer: `0z` prefix for compressed binary literals

- [ ] In `readNumber()` (bbl.cpp ~line 218), after the `0b` check, add:
  ```cpp
  if (!isNegative && peek() == '0' && pos + 1 < len && src[pos + 1] == 'z') {
      return readCompressedBinary();
  }
  ```
- [ ] Add `readCompressedBinary()` method to `BblLexer` (bbl.h ~line 400). Implementation is identical to `readBinary()` except: skip `0z` instead of `0b`, set `t.isCompressed = true`.
- [ ] Add `bool isCompressed = false` field to `Token` struct (bbl.h ~line 387).
- [ ] Add `bool isCompressed = false` field to `AstNode` struct (bbl.h ~line 449).
- [ ] In `parsePrimary()` (bbl.cpp ~line 405), for `TokenType::Binary`, also copy `tok.isCompressed` to `node.isCompressed`.

## BblBinary: compressed lazy materialize

- [ ] Add `bool compressed = false` field to `BblBinary` (bbl.h ~line 100).
- [ ] Update `materialize()`: when `compressed` is true, use `LZ4F_decompress()` to decompress `lazySource`/`lazySize` into `data`. Include `<lz4frame.h>`.
- [ ] Update `length()`: when `compressed && lazySource`, call `LZ4F_getFrameInfo()` to read content size from the LZ4 frame header (first ~15 bytes). Return that without full decompression.
- [ ] Add `BblBinary* allocLazyBinary(const char* src, size_t size, bool compressed)` — overload or add bool param to existing `allocLazyBinary`.

## Compiler: propagate compressed flag

- [ ] In compiler.cpp `NodeType::BinaryLiteral` case (~line 82), pass `node.isCompressed` to `allocLazyBinary()`:
  ```cpp
  BblBinary* bin = node.binarySource
      ? state.allocLazyBinary(node.binarySource, node.binarySize, node.isCompressed)
      : state.allocBinary(node.binaryData);
  ```

## CLI: `bbl --compress`

- [ ] In `main.cpp`, add `--compress` flag handling. When set:
  1. Read entire stdin into a `std::string source`.
  2. Create `BblLexer lexer(source.c_str())`.
  3. Iterate tokens. For each token:
     - If `TokenType::Binary` (an `0b` literal): LZ4-compress `tok.binarySource`/`tok.binarySize` using `LZ4F_compressFrame()`. Output `0z<compressed_size>:` followed by compressed bytes.
     - Otherwise: output the original source text from the previous token end to this token end (preserving all non-binary content verbatim).
  4. Write to stdout.
- [ ] This requires tracking source positions per token. Add `int startPos` and `int endPos` to `Token` so the CLI can copy verbatim ranges.

## C++ API: compress/decompress helpers

- [ ] Add to bbl.h / bbl.cpp:
  ```cpp
  namespace BBL {
      std::vector<uint8_t> compress(const uint8_t* data, size_t size);
      std::vector<uint8_t> decompress(const uint8_t* data, size_t size);
  }
  ```
- [ ] `compress`: use `LZ4F_compressFrame()` with `contentSize` set in preferences so decompressed size is in the frame header.
- [ ] `decompress`: use `LZ4F_decompress()` in a loop, growing the output buffer.

## BBL stdlib: compress/decompress functions

- [ ] In `addStdLib()` (bbl.cpp), register:
  - `compress`: takes BblBinary arg, returns new BblBinary with LZ4-compressed data.
  - `decompress`: takes BblBinary arg, returns new BblBinary with decompressed data.
- [ ] These use `BBL::compress` / `BBL::decompress` internally.

## Tests

- [ ] Unit test: create a `0z` literal in source, verify `:length` returns decompressed size, `:at 0` returns correct first byte.
- [ ] Unit test: `(compress (binary v))` followed by `(decompress result)` matches original.
- [ ] Round-trip test: take a .bbl with `0b` binary, run `bbl --compress`, execute the compressed output, verify same result as original.
- [ ] Verify all 736 existing tests still pass.
- [ ] Verify all 24 functional tests still pass.
