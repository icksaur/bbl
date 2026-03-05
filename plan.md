# plan

This document must only contain the next actions, no cut or deferred work.
The items in this plan must be actionable by another agent without guesswork.
When all items are complete, remove all items.

legend:

[ ] incomplete
[*] complete

---

All items complete. Compressed binary blobs implemented.

[*] CMake: vendor LZ4 static library
[*] Lexer: `0z` prefix for compressed binary literals
[*] BblBinary: compressed lazy materialize
[*] Compiler: propagate compressed flag
[*] CLI: `bbl --compress`
[*] C++ API: compress/decompress helpers
[*] BBL stdlib: compress/decompress functions
[*] Tests: all 736 unit + 24 functional pass, round-trip verified
