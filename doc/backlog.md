# Backlog

---
- **struct methods** — Register named functions on `StructDesc` (like userdata methods). `(v:length)` looks up `"length"` in the descriptor's method table and calls it with the struct instance as the first argument. Struct binary layout stays POD — no function storage in the instance data. Follows the existing userdata method pattern.