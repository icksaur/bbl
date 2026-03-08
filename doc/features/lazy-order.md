# Lazy Order Tracking for Tables
Status: proposed

## Goal

Remove per-insert overhead from BblTable::set() by deferring order vector construction until a consumer requests it.

## Background

BblTable::set() allocates `order = new std::vector<BblValue>()` on first insert and `push_back`s every new key. For 100k inserts this means 100k push_backs + ~17 reallocations. LuaJIT doesn't track insertion order at all.

Consumers of `order`: `keys()`, `pop()`, `at()`, JSON serialization, `serializeMessage`.

## Design

1. Remove `order->push_back(key)` from `BblTable::set()`
2. Remove order cleanup from `BblTable::del()`
3. Add `void BblTable::ensureOrder()` — scans buckets to build order on demand
4. Call `ensureOrder()` in all consumers before accessing `order`

`ensureOrder()` scans occupied buckets and builds the order vector. This is O(capacity) but only runs when a consumer needs it, not on every insert.

Note: insertion order is lost — `ensureOrder()` returns bucket-scan order. This is acceptable because insertion order was never guaranteed in the language spec, and the primary use case (sequential integer keys) iterates via `nextIntKey` anyway.

## Acceptance

- BblTable::set() does not touch `order`
- BblTable::del() does not touch `order`  
- `keys()`, `pop()`, `at()`, JSON serialization still work correctly
- All 769 unit + 35 functional tests pass
- table_heavy benchmark improves
