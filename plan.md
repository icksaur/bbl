# backlog

- Parser: skip binary data literals — lazy-load from file. Don't parse binary blobs inline; defer loading until accessed.
- Binary compression: `0z1234:<binary>` syntax for compressed (zipped) binary data. Decompress on first access.
