# Integer Dot Syntax
Status: done

## Goal

Allow integer literals after `.` on vectors and tables, providing concise positional read/write access without calling `at`/`set`/`get` methods.

```bbl
(= v (vector int 10 20 30))
(print v.0)       // 10 — sugar for (v.at 0)
(= v.0 99)        // write — sugar for (v.set 0 99)
(print v.0)       // 99

(= t (table))
(t.push "hello")
(print t.0)       // "hello" — sugar for (t.get 0)
(= t.0 "world")   // write — sugar for (t.set 0 "world")
```

## Design

### Parser change

The parser's dot-access loop (`parseExpr`) currently requires a `Symbol` token after `.`.  Change: also accept `Int` tokens.

When the token after `.` is `Int`:
- Create a `DotAccess` node as usual.
- Store the integer in the node's `intVal` field.
- Set `stringVal` to the empty string `""` as a sentinel — normal dot-access always has a non-empty `stringVal`, so empty means "integer index".

No new `NodeType` needed.  The existing `AstNode` struct already has both `stringVal` and `intVal`.

### Chaining

Chained access works naturally because the parser's `while` loop already chains `DotAccess` nodes:

```bbl
(= v (vector vertex (vertex 1 2 3)))
(print v.0.x)     // DotAccess(int 0) → DotAccess(symbol "x")
```

The inner `v.0` evaluates to a struct value, then the outer `.x` reads a field — no special handling needed.

### Eval — read path

In the `DotAccess` eval handler, when `stringVal` is empty (integer-dot):

- **Vector**: call `readVecElem(vec, intVal)`.  This check must come *before* the existing vector branch (which unconditionally throws "vector methods must be called, not accessed as fields").
- **Table**: call `table->get(BblValue::makeInt(intVal))`.  Must come before the method-first resolution and string-key fallback.
- **Other types**: throw an error — integer-dot is meaningless on structs, strings, etc.

### Eval — write path (place expression)

In the `=` special form's `DotAccess` branch, when `target.stringVal` is empty (integer-dot):

- **Vector**: call `writeVecElem(vec, intVal, val)`.  The write path currently has no vector branch at all — this is a new case, not a modification.
- **Table**: call `table->set(BblValue::makeInt(intVal), val)`.
- **Other types**: throw an error.

### Sentinel invariant

`stringVal == ""` distinguishes integer-dot from symbol-dot.  No other code path produces a `DotAccess` node with empty `stringVal` — the parser's symbol branch always fills `stringVal` from a `Symbol` token (which is never empty).

### Call position

`(v.0 args...)` in the DotAccess method-call handler: integer-dot in call position is an error.  The method name will be `""` which won't match any method, so the existing "has no method" throw handles this naturally.

## Considerations

### Float ambiguity — none

`v.0` tokenizes as `Symbol("v")`, `Dot`, `Int(0)`.  The tokenizer only absorbs `.` into a number when it's already *inside* a number token.  Since `v` is a symbol, the dot is consumed separately.  No ambiguity.

### Negative indices — not supported

`v.-1` would tokenize as `Symbol("v")`, `Dot`, `-`, `Int(1)` — three separate tokens.  Negative indices don't make sense for vectors (out-of-bounds) and aren't meaningful for tables.  Not a concern.

### Computed indices — use methods

Integer-dot is literal-only syntactic sugar.  For computed indices, use `v.at`, `v.set`, `t.get`, `t.set`.  This is consistent with how `t.name` is literal string sugar while `(t.get key)` accepts variables.

### gatherFreeVars — no change needed

`gatherFreeVars` already recurses into `DotAccess` children.  The integer index is stored in `intVal`, not as a child node.

## Acceptance

1. `v.0` reads vector element 0.
2. `(= v.0 val)` writes vector element 0.
3. `t.0` reads table integer key 0.
4. `(= t.0 val)` writes table integer key 0.
5. Chaining works: `v.0.x` reads field `x` of vector element 0.
6. Out-of-bounds `v.5` on a 3-element vector throws a runtime error.
7. Integer-dot on non-container types (struct, string, int) throws an error.
8. All existing tests still pass.
9. Feature spec updated to status: done.
10. `doc/bbl.md` and `doc/spec.md` document the syntax.
