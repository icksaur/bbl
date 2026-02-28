# BBL Implementation Notes

Internal architecture, performance characteristics, and design decisions.

---

## Memory Model

### Garbage Collection

Mark-and-sweep.  All GC-managed types (string, binary, fn, vector, table,
userdata) are allocated through `BblState` and tracked in allocation pools.

**Collection cycle:**
1. **Mark** -- starting from roots, traverse all reachable objects, set mark bit
2. **Sweep** -- walk allocation list, use `std::partition` to move live objects
   to front, delete dead tail, clear mark bits on survivors

**Roots:**
- Scope chain (all live bindings from root through call stack)
- String intern table
- Type descriptor table
- Active function call scopes (slots and bindings)

**Trigger:** allocation pressure.  After N new allocations since last GC
(controlled by `gcThreshold`), collection runs at the next safe point.

**Safe points:** top of `exec`/`execExpr`, loop iterations.  GC never triggers
mid-expression -- all live values are rooted in scope bindings at safe points.

**Adaptive threshold:** after each sweep, `gcThreshold = max(256, liveCount * 2)`.
Reduces GC frequency in allocation-heavy scripts.

**Implications:**
- No deterministic destruction -- use `with` for resources like file handles
- No cycle problems -- GC handles circular references naturally
- Assignment is cheap -- pointer copy, no refcount overhead

### String Interning

All strings live in a global intern table on `BblState`, keyed by content hash.
Duplicate strings share one allocation.

**GC integration:** strings are added to both the intern table and the GC
allocation pool.  During sweep, unmarked strings are removed from both.  Pointer
equality is preserved (deduplication maintained).

**Performance:** O(1) amortized for interning (hash lookup).  String equality
comparison is O(1) via pointer compare.

### Scope Chain

One scope type: `BblScope` -- a symbol-to-value table with a parent pointer.

**Fresh scope** (new table): `fn` calls, `execfile` from script, `exec` from
script.

**Shared scope** (runs in enclosing table): `do`, `loop`, `if`, `each`.

Variable lookup walks the parent-pointer chain calling
`std::unordered_map::find()` at each level.  O(depth * hash) for deep nesting,
but most scripts have shallow scopes (3-5 levels).

**Function scopes** use a flat slot-based layout (see Performance section) with
no parent chain.

### Closure Capture

When `fn` is evaluated, `gatherFreeVars()` performs an AST walk to find free
variables.  Each free variable's current value is copied into the function's
capture environment (`vector<pair<uint32_t, BblValue>>`).

**Value types** (int, float, bool, null, struct): snapshot copy.  Independent
of original.

**GC types** (string, binary, fn, vector, table, userdata): shared pointer.
Same object; mutations visible everywhere.

**Rebinding** the outer variable after capture has no effect on the closure --
it holds the value from capture time.

**Self-capture:** after `(= name (fn ...))`, the function's own name is injected
into its captures.  Enables direct recursion.  Mutual recursion requires a table
workaround.

**`=` inside closures:** modifies the captured binding slot, not the original
scope.  For value types, this means the closure's copy.  For GC types, the
binding changes but the underlying object is still shared.

---

## Evaluation

### AST Tree-Walking Interpreter

Every expression goes through `eval()` -> `switch` on node type.  Function
calls re-evaluate the body AST each time.  This is the dominant cost in
compute-heavy scripts.

**Architectural limit:** a bytecode VM would be 5-20x faster but is out of
scope.

### Special Form Dispatch

Special forms (`=`, `if`, `loop`, `fn`, `and`, `or`, etc.) are dispatched via
a static `unordered_map<string, SpecialFormHandler>` built at startup.  O(1)
lookup instead of sequential `if (op == "...")` chains.

Additionally, `AstNode` caches its resolved special form ID
(`mutable int8_t cachedSpecialForm`) to skip the hash lookup on repeat
evaluations (e.g., loop bodies).

### Function Call Execution

1. Arity check
2. Evaluate arguments (stack-local 8-element buffer avoids heap alloc for most
   calls)
3. Create fresh scope with flat slot layout
4. Load captures into slots `[0, N)`
5. Bind arguments into slots `[paramSlotStart, paramSlotStart + M)`
6. Evaluate body
7. Pop scope

**Flat scope:** function scopes use a `vector<BblValue>` indexed by slot ID
instead of a hash map.  Slot layout is computed once at `fn` creation time.
Most lookups become O(1) array indexing.

### Method Dispatch

**Colon access** (`obj:method args`):
1. Evaluate `obj`
2. Switch on type tag
3. Look up method name in type-specific dispatch (built-in method table for
   vector/string/binary, `UserDataDesc::methods` for userdata, built-in then
   key lookup for tables)
4. Evaluate arguments
5. Call method with object as implicit first argument

**Dot access** (`obj.field`):
- Struct: field read via `FieldDesc` offset
- Table: string key lookup
- Vector/table: integer index read

### Symbol Resolution

Symbols are assigned numeric IDs (`uint32_t`) at parse time.  The intern table
maps strings to IDs; scope bindings use integer keys.  This eliminates string
hashing on every variable access.

`AstNode` caches `symbolId` (mutable field) so that parse-time resolution is
amortized across repeated evaluations.

### Flow Control Signals

`break` and `continue` use a state flag (`flowSignal`) on `BblState` rather
than exceptions.  Values: `FlowNone=0`, `FlowBreak=1`, `FlowContinue=2`.

- `break` sets signal and returns immediately; loop checks after each iteration
- `continue` sets signal; loop resets it and skips to condition check
- Inside `with`, signals propagate correctly (cleanup runs first)
- Inside `fn`, stray signals are reset with an error

---

## Performance

### Benchmark Results

Release build, GCC, Linux x86_64.  Three rounds of optimization from baseline.

| Benchmark                    | Baseline | Final   | Speedup |
|------------------------------|----------|---------|---------|
| loop_arith (1M iterations)   | 0.286s   | 0.065s  | 4.4x    |
| function_calls (500K calls)  | 0.220s   | 0.048s  | 4.6x    |
| gc_pressure (100K allocs)    | 0.030s   | 0.010s  | 3.0x    |
| string_intern (10K concats)  | 0.058s   | 0.057s  | ~1.0x   |
| table_heavy (1K entries)     | 0.003s   | 0.002s  | 1.5x    |

### Optimizations Applied

**Round 1 (2.1-2.8x from baseline):**
1. GC sweep partition (`std::partition` instead of erase -- O(n) vs O(n^2))
2. Adaptive GC threshold (`max(256, liveCount * 2)`)
3. Special-form dispatch table (O(1) hash vs sequential if-chain)
4. Parse-time string intern caching (cache `BblString*` on AST node)
5. Symbol IDs (`uint32_t` keys instead of `std::string` for bindings)
6. Flat scope for function calls (`vector<BblValue>` slots instead of hash map)

**Round 2 (+1.17-1.44x from R1):**
7. Cached SpecialForm on AstNode (`mutable int8_t`)
8. Stack-buffer for function args (avoid heap alloc for <= 8 args)
9. Direct param slot indexing (`paramSlotStart` on `BblFn`)

**Round 3 (+1.13-1.20x from R2):**
10. Cached symbolId on set/def targets (eliminate string hashing on assignment)
11. Integer fast-path for comparisons (avoid int->double conversion)
12. Lazy bindings map initialization (`unique_ptr`, allocate on first use)

### Known Characteristics

**Eval dispatch** is the primary bottleneck: ~65ns/iteration for simple
arithmetic.  Inherent to tree-walking interpretation.

**Function call overhead:** ~96ns/call including scope setup and capture loading.

**Table operations:** O(n) linear scan for get/set/has/del.  Fine for small
tables (< 50 entries), which is the typical case.  For large collections, use
vectors with integer indexing.

**String concat:** O(n^2) for repeated concatenation of growing strings.  Use
`fmt` for building complex strings.  Uncommon in the target use case (game asset
serialization).

### Profiling

```sh
cmake -B build_rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_rel
perf record ./build_rel/bbl tests/bench/loop_arith.bbl
perf report    # focus on top-5 hotspots
hyperfine './build_rel/bbl tests/bench/loop_arith.bbl'
```

---

## Error System

### Error Type

```cpp
struct BBL::Error { std::string what; };
```

Single type for all runtime errors.  Thrown as C++ exception, caught by the host.

### Call Stack

`BblState` maintains a call stack of `Frame` structs:

```cpp
struct Frame { std::string file; int line; std::string expr; };
```

Pushed on function entry, popped on exit.  `printBacktrace()` walks the stack
innermost-first, printing file, line, and abbreviated expression to stderr.

### Script-Level Error Handling

`try`/`catch` catches `BBL::Error` in script.  The error message is bound to
the catch variable.  Does not catch C++ exceptions.

---

## Type Descriptor System

### Struct Descriptors

`StructDesc` holds field layout: name, byte offset, size, C type, optional
nested struct type name.  Registered via `StructBuilder`, stored globally on
`BblState`.

**Validation at registration:**
- No overlapping fields
- No size overflow beyond `totalSize`
- Field types must be valid `CType` values
- Re-registration: same layout is a silent no-op; different layout is an error

**Field access:** `readField` reads bytes at offset, widens narrow types to
int64/double.  `writeField` narrows from int64/double and writes at offset.

### UserData Descriptors

`UserDataDesc` holds method table (`unordered_map<string, BblCFunction>`) and
optional destructor.  Registered via `TypeBuilder`.

**Method resolution:** colon access on userdata looks up method name in the
descriptor's method table.  Unknown method name throws.

### Type Tags vs Descriptors

Every `BblValue` carries a `BBL::Type` tag (integer enum).  Struct and userdata
values additionally carry a pointer to their descriptor.  Descriptor lookup is
O(1) -- no hash, no string comparison.

---

## Lexer / Parser

### Tokenization

`BblLexer` is a single-pass character scanner producing tokens.  Key behaviors:

- **Numbers:** integers and floats distinguished by presence of `.`.  No hex, no
  suffixes.  Negative numbers are separate `-` token + number token (handled by
  parser).
- **Strings:** standard double-quoted with `\"`, `\\`, `\n`, `\t` escapes.
- **Binary literals:** `0b<size>:<bytes>` -- lexer reads the size as decimal
  integer, then reads exactly that many raw bytes.
- **Symbols:** start with letter, `_`, or `-`; can contain digits after first
  char.
- **Dot/Colon:** single-character tokens.  After dot, integers are accepted
  (for integer dot notation like `v.0`).

### AST

`AstNode` is the universal AST node.  Node types:

| NodeType        | Content                                     |
|-----------------|---------------------------------------------|
| IntLiteral      | `intVal`                                    |
| FloatLiteral    | `floatVal`                                  |
| StringLiteral   | `stringVal`                                 |
| BoolLiteral     | `boolVal`                                   |
| NullLiteral     | --                                          |
| BinaryLiteral   | `binaryData`                                |
| Symbol          | `stringVal`, `symbolId` (cached)            |
| List            | `children` (operator + operands)            |
| DotAccess       | `children[0]` = object, `stringVal` = field |
| ColonAccess     | `children[0]` = object, `stringVal` = method|

DotAccess with `stringVal == ""` and `intVal` set indicates integer dot notation.

Mutable cached fields: `symbolId` (uint32_t) and `cachedSpecialForm` (int8_t)
are resolved on first evaluation and reused.

---

## Code Quality Principles

- **Worst:** complexity, coupling, wrong abstraction
- **Bad:** side effects, global state, unnecessary abstraction, mutable objects,
  sync drift between docs and code
- **Good:** strong typing, unit tests, layering, minimal code, SRP, functional
  procedures, immutable objects, descriptive naming, data-driven behavior,
  encapsulation over inheritance
- Prefer `const`/`constexpr`, value semantics, `std::string_view`
- Prefer `std::variant`/`optional`/`span` over raw pointers
- Avoid raw `new`/`delete`
- One class = one purpose
- Comments explain **why**, not **what** -- names explain what
- No commented-out code, no TODO comments

### Style

- Types/classes/enums: `UpperCamelCase`
- Variables/parameters/members: `lowerCamelCase`
- Functions/methods: `lowerCamelCase`
- Enum values: `UpperCamelCase`
- Files: lowercase, no separators
- K&R braces, 4-space indent, always braces even for single statements
- Private members first (no `private:` label), then public section
