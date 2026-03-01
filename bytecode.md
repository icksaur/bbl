# Bytecode VM
Status: proposed

## Goal

Replace the tree-walking AST interpreter with a stack-based bytecode virtual
machine.  The parser is unchanged -- it produces the same `AstNode` trees.  A
new compiler walks those trees and emits compact bytecode.  A new VM executes
that bytecode in a tight dispatch loop.  Expected result: 5-15x faster
execution on compute-heavy scripts with no change to language semantics.

---

## Background

### Current execution model

BBL evaluates code by walking AST nodes.  `eval()` switches on the 10 node
types.  `List` nodes dispatch through 38 special forms via a hash-map lookup
(cached to `int8_t` on the AST node after first hit).  Function bodies are
stored as AST and re-traversed on every call.  Loop bodies re-traverse the
same nodes on every iteration.

This is the dominant cost.  Profiling shows:

| Benchmark                  | Time    | Per-op cost |
|----------------------------|---------|-------------|
| loop_arith (1M iterations) | 0.065s  | ~65 ns      |
| function_calls (500K)      | 0.048s  | ~96 ns      |
| gc_pressure (100K allocs)  | 0.010s  | ~100 ns     |

Three rounds of optimization (flat scope slots, cached symbol IDs, integer
fast-paths, stack arg buffers) yielded 4.4x improvement from baseline.  The
remaining bottleneck -- eval dispatch + pointer chasing through AST nodes --
is architectural and cannot be optimized further within a tree-walker.

### Why bytecode is faster

1. **Cache locality.**  Bytecode is a dense, contiguous byte array.  The CPU
   prefetches it naturally.  AST nodes are scattered heap objects connected
   by pointers -- every child access risks a cache miss.

2. **Single dispatch.**  One switch per instruction, not two (node type +
   special form).  Computed-goto dispatch eliminates even the switch overhead.

3. **No re-traversal.**  A function's bytecode is compiled once and executed
   on every call.  The tree-walker re-walks the AST every time.

4. **Compact representation.**  `(+ a 1)` is 3 AST nodes (~300+ bytes with
   string data, vectors, caches).  As bytecode: `OP_GET_LOCAL 0x00 0x00
   OP_CONST 0x00 0x00 OP_ADD` = 7 bytes.

### Why stack-based (not register-based)

Six approaches were evaluated: stack VM, register VM (Lua-style), computed
goto, direct threading, NaN-boxing, and JIT compilation.

Register-based VMs execute fewer instructions per operation (~20-30% fewer
dispatches) but require register allocation in the compiler and more complex
instruction encoding (32-bit fixed-width words with bitfield packing).

Stack-based VMs are simpler to compile (post-order AST traversal emits
push/op/pop sequences with no allocation decisions) and have a trivial
calling convention.  BBL's existing flat-slot scope model maps directly to
stack-frame locals at known offsets.

The stack VM is the right trade-off: simplest implementation, well-understood
design (clox, CPython, Ruby YARV), and sufficient performance for a
serialization scripting language.  If more speed is ever needed, the bytecode
format can be evolved to register-based without changing the rest of the
system.

### Approaches rejected

**Register-based VM.**  8-20x speedup but ~2500 LOC and requires a register
allocator.  Not justified given BBL's use case.

**NaN-boxing.**  Encodes values in 64-bit NaN payloads (8 bytes vs 24).
Incompatible with BBL's 64-bit signed integers -- they would need heap boxing,
losing the performance benefit.

**JIT compilation.**  50-100x speedup but 10,000+ LOC, platform-specific, and
completely overkill for game asset serialization scripts.

**Direct threading.**  Instruction stream stores function pointers instead of
byte opcodes -- 8x wider, not serializable, not portable across builds.

---

## Design

### Compilation pipeline

```
source text
    |  (unchanged)
    v
  Parser  -->  AstNode tree
    |  (new)
    v
 Compiler  -->  Chunk (bytecode + constants)
    |  (new)
    v
    VM     -->  BblValue result
```

The parser is not modified.  The compiler is a new single-pass recursive
descent over the AST.  The VM is a new dispatch loop.

### Chunk

A chunk is the unit of compiled code.  Each function body compiles to one
chunk.  The top-level script is also a chunk (wrapped in an implicit function).

```cpp
struct Chunk {
    std::vector<uint8_t> code;       // bytecode stream
    std::vector<BblValue> constants; // constant pool
    std::vector<int> lines;          // source line per byte (debug/errors)
};
```

**Constants** are values too large or complex to encode inline: 64-bit
integers, 64-bit floats, interned strings, and compiled function prototypes.
Each constant is referenced by a 16-bit index, supporting up to 65,536
constants per chunk.

**Line numbers** map each bytecode byte back to its source line for error
reporting and backtraces.  This is a simple parallel array (same length as
`code`).  Run-length encoding can be added later if memory matters.

### Value representation

No change.  `BblValue` remains a tagged union (type enum + 8-byte union,
~24 bytes total).  NaN-boxing was considered and rejected due to BBL's 64-bit
integer requirement.

### Instruction encoding

One-byte opcodes, variable-length operands.  This is the simplest encoding and
the most common in production VMs (CPython, Ruby YARV, clox).

Operand widths:
- `u8`: 1-byte unsigned (argument counts, upvalue indices)
- `u16`: 2-byte unsigned big-endian (constant indices, local slots, jump offsets)

The 16-bit operand width supports up to 65,536 locals and constants per
function.  This is more than sufficient -- BBL functions are typically small.

### Instruction set

~50 opcodes organized by category.  The enum values are assigned sequentially
starting from 0.

```cpp
enum OpCode : uint8_t {
    // -- Constants & Literals --
    OP_CONSTANT,        // <u16 idx>     push constants[idx]
    OP_NULL,            //               push null
    OP_TRUE,            //               push true
    OP_FALSE,           //               push false

    // -- Stack --
    OP_POP,             //               discard top
    OP_DUP,             //               duplicate top
    OP_POPN,            // <u8 n>        discard top n values

    // -- Arithmetic --
    OP_ADD,             //               pop b, a; push a + b (int fast-path)
    OP_SUB,             //               pop b, a; push a - b
    OP_MUL,             //               pop b, a; push a * b
    OP_DIV,             //               pop b, a; push a / b
    OP_MOD,             //               pop b, a; push a % b

    // -- Bitwise --
    OP_BAND,            //               pop b, a; push a & b
    OP_BOR,             //               pop b, a; push a | b
    OP_BXOR,            //               pop b, a; push a ^ b
    OP_BNOT,            //               pop a; push ~a
    OP_SHL,             //               pop b, a; push a << b
    OP_SHR,             //               pop b, a; push a >> b

    // -- Comparison --
    OP_EQ,              //               pop b, a; push a == b
    OP_NEQ,             //               pop b, a; push a != b
    OP_LT,              //               pop b, a; push a < b
    OP_GT,              //               pop b, a; push a > b
    OP_LTE,             //               pop b, a; push a <= b
    OP_GTE,             //               pop b, a; push a >= b

    // -- Logical --
    OP_NOT,             //               pop a; push !a

    // -- Variables --
    OP_GET_LOCAL,       // <u16 slot>    push stack[frame + slot]
    OP_SET_LOCAL,       // <u16 slot>    stack[frame + slot] = peek(0)
    OP_GET_CAPTURE,     // <u8 idx>      push closure.captures[idx]
    OP_SET_CAPTURE,     // <u8 idx>      closure.captures[idx] = peek(0)
    OP_GET_GLOBAL,      // <u16 idx>     push globals[constants[idx]]
    OP_SET_GLOBAL,      // <u16 idx>     globals[constants[idx]] = peek(0)

    // -- Control Flow --
    OP_JUMP,            // <i16 offset>  ip += offset
    OP_JUMP_IF_FALSE,   // <i16 offset>  if falsy(peek(0)): ip += offset; pop
    OP_JUMP_IF_TRUE,    // <i16 offset>  if truthy(peek(0)): ip += offset; pop
    OP_LOOP,            // <u16 offset>  ip -= offset (backward jump)

    // -- Short-circuit --
    OP_AND,             // <i16 offset>  if falsy(peek(0)): ip += offset; else pop
    OP_OR,              // <i16 offset>  if truthy(peek(0)): ip += offset; else pop

    // -- Functions & Calls --
    OP_CLOSURE,         // <u16 idx> ... create closure from constants[idx]
    OP_CALL,            // <u8 argc>     call stack[top - argc - 1](args)
    OP_TAIL_CALL,       // <u8 argc>     tail-call: reuse current frame
    OP_RETURN,          //               return top of stack to caller

    // -- Collections --
    OP_VECTOR,          // <u16 count>   pop count values; push new vector
    OP_TABLE,           // <u16 count>   pop count*2 values (k,v); push table
    OP_STRUCT,          // <u16 idx> <u8 argc>  construct struct; type = const[idx]
    OP_BINARY,          //               pop arg; construct binary (branch on type)

    // -- Field & Index Access --
    OP_GET_FIELD,       // <u16 idx>     pop obj; push obj.field (name = const[idx])
    OP_SET_FIELD,       // <u16 idx>     pop val, obj; obj.field = val
    OP_GET_INDEX,       //               pop idx, obj; push obj[idx]
    OP_SET_INDEX,       //               pop val, idx, obj; obj[idx] = val
    OP_METHOD_CALL,     // <u16 idx> <u8 argc>  pop obj+args; call obj:method

    // -- Exception Handling --
    OP_TRY_BEGIN,       // <u16 offset>  push handler (catch at ip + offset)
    OP_TRY_END,         //               pop handler; jump past catch block

    // -- Resource Management --
    OP_WITH_BEGIN,      //               peek resource; push cleanup marker
    OP_WITH_END,        //               pop marker; call resource destructor

    // -- Misc --
    OP_LENGTH,          //               pop obj; push obj length (int)
    OP_SIZEOF,          // <u16 idx>     push sizeof struct (name = const[idx])
    OP_EXEC,            //               pop string; compile and run in fresh scope
    OP_EXECFILE,        //               pop path; load, compile, run in root scope
};
```

### Special form compilation

Each of BBL's 38 special forms compiles to one or more opcodes.  None require
runtime dispatch -- the compiler resolves them statically.

`typeof` is **not** a special form -- it is a C function registered via
`defn()`.  It compiles as a normal function call (`OP_GET_GLOBAL` +
`OP_CALL`), preserving its identity as a first-class callable value.

BBL has no unary minus operator.  Negative numeric literals are handled by the
parser.  `(- x)` is not valid BBL, so no `OP_NEG` opcode is needed.

**Variadic forms:** `+`, `-`, `*`, `/`, `%`, `band`, `bor`, `bxor` accept 2+
arguments.  The compiler emits a chain of binary ops: `compile(a), compile(b),
OP_ADD, compile(c), OP_ADD, ...`.  This preserves left-to-right evaluation and
handles string concatenation (`(+ "a" "b" "c")`).

| Special form      | Compilation                                          |
|-------------------|------------------------------------------------------|
| `+` `-` `*` `/` `%` | compile(a), compile(b), OP_ADD, [compile(c), OP_ADD]... (chained binary) |
| `==` `!=` `<` `>` `<=` `>=` | compile(lhs), compile(rhs), emit OP_EQ/... |
| `band` `bor` `bxor` | compile(a), compile(b), OP_BAND, [compile(c), OP_BAND]... (chained) |
| `shl` `shr`       | compile(lhs), compile(rhs), emit OP_SHL/SHR          |
| `bnot`            | compile(arg), emit OP_BNOT                           |
| `not`             | compile(arg), emit OP_NOT                            |
| `=` (symbol)      | compile(value), emit OP_SET_LOCAL/CAPTURE/GLOBAL (see assign-or-create below) |
| `=` (dot target)  | compile(value), compile(obj), emit OP_SET_FIELD `<nameIdx>` |
| `=` (index target)| compile(value), compile(obj), compile(idx), emit OP_SET_INDEX |
| `if`              | compile(cond), OP_JUMP_IF_FALSE(else), compile(then), OP_JUMP(end), compile(else) |
| `loop`            | mark(start), compile(cond), OP_JUMP_IF_FALSE(exit), compile(body), OP_POP, OP_LOOP(start), patch(exit), OP_NULL |
| `each`            | compile(container), OP_DUP, OP_LENGTH, init i=0 local, loop: compare i < len, OP_JUMP_IF_FALSE(exit), OP_GET_INDEX, bind element, compile(body), OP_POP, incr i, OP_LOOP(start) |
| `do`              | compile each child sequentially, OP_POP between, keep last value |
| `and`             | compile(lhs), OP_AND(skip), compile(rhs)             |
| `or`              | compile(lhs), OP_OR(skip), compile(rhs)              |
| `fn`              | compile body into new Chunk, emit OP_CLOSURE         |
| `break`           | emit OP_JUMP to loop exit (patched)                  |
| `continue`        | emit OP_LOOP to loop start                           |
| `try`             | emit OP_TRY_BEGIN(catch), compile body, OP_TRY_END, compile catch |
| `with`            | compile resource, emit OP_WITH_BEGIN, compile body, OP_WITH_END |
| `vector`          | compile elements, emit OP_VECTOR                     |
| `table`           | compile pairs, emit OP_TABLE                         |
| `struct` (declare)| register StructDesc at compile time; emit OP_NULL (no runtime work) |
| `struct` (construct)| compile field values, emit OP_STRUCT `<typeIdx>` `<argc>` |
| `binary`          | compile arg, emit OP_BINARY                          |
| `sizeof`          | emit OP_SIZEOF                                       |
| `exec`            | compile string, emit OP_EXEC                         |
| `execfile`        | compile path, emit OP_EXECFILE                       |

**`loop` result value:** loops return null.  After the exit jump target, the
compiler emits `OP_NULL` to ensure a null value is on the stack as the loop's
result.  `each` follows the same pattern.

**`struct` as declaration vs construction:** the `struct` special form has two
roles.  `(struct Name type1 field1 ...)` is a type declaration -- the compiler
processes the field descriptors at compile time (registering a `StructDesc` on
`BblState`) and emits only `OP_NULL`.  `(TypeName arg1 arg2 ...)` is
construction -- the compiler compiles argument values and emits `OP_STRUCT`.
The VM reads the field count from the `StructDesc` to validate argc.

**Assign-or-create semantics for `=`:** the compiler tracks which local names
have been declared in the current scope.  The first `=` to a new name
allocates a local slot (implicit definition).  Subsequent `=` to the same name
emits `OP_SET_LOCAL` to the existing slot.  This matches the tree-walker's
assign-or-create behavior.  For names not found in any enclosing compiler
scope, `OP_SET_GLOBAL` is emitted.

### VM state

```cpp
constexpr int MAX_FRAMES = 256;
constexpr int MAX_STACK  = MAX_FRAMES * 256;  // 65,536 slots

struct CallFrame {
    Chunk* chunk;           // bytecode being executed
    uint8_t* ip;            // instruction pointer into chunk->code
    BblValue* slots;        // pointer into value stack (frame base)
    BblClosure* closure;    // closure for capture access (null for top-level)
    int line;               // source line at entry (for backtraces)
};

// VM state lives inside BblState alongside existing GC/intern infrastructure
struct VmState {
    CallFrame frames[MAX_FRAMES];
    int frameCount = 0;
    BblValue stack[MAX_STACK];
    BblValue* stackTop = stack;
    std::unordered_map<uint32_t, BblValue> globals;  // symbol ID -> value
};
```

The VM state is embedded in `BblState`.  The GC, string intern table, type
descriptors, and C function registry remain exactly as they are.  The VM's
value stack becomes an additional GC root (scanned during mark phase).

**Globals table:** `VmState::globals` maps symbol IDs to values.  C functions
registered via `defn()` are inserted into globals at registration time.  C API
`set()`/`get()` operations also read/write globals.  `OP_GET_GLOBAL` and
`OP_SET_GLOBAL` use the constant pool to resolve the symbol name to an ID,
then look up the globals table.

**`OP_EXEC` scope:** creates a fresh global scope (matching tree-walker
behavior where `exec` from script gets an isolated scope).  The executed code
cannot see the calling function's locals or captures.

**`OP_EXECFILE` scope:** accumulates definitions into the VM's globals table
(matching tree-walker behavior where `execfile` populates the root scope).

### Dispatch loop

The core loop reads one opcode per iteration and switches on it.  This is the
single hottest function in the system.

```cpp
InterpretResult run(BblState& state) {
    CallFrame* frame = &state.vm.frames[state.vm.frameCount - 1];

    #define READ_BYTE()  (*frame->ip++)
    #define READ_U16()   (frame->ip += 2, \
        (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_CONST() (frame->chunk->constants[READ_U16()])
    #define PUSH(v)      (*state.vm.stackTop++ = (v))
    #define POP()        (*--state.vm.stackTop)
    #define PEEK(n)      (state.vm.stackTop[-1 - (n)])

    for (;;) {
        uint8_t op = READ_BYTE();
        switch (op) {

        case OP_CONSTANT: PUSH(READ_CONST()); break;
        case OP_NULL:     PUSH(BblValue::makeNull()); break;
        case OP_TRUE:     PUSH(BblValue::makeBool(true)); break;
        case OP_FALSE:    PUSH(BblValue::makeBool(false)); break;
        case OP_POP:      POP(); break;

        case OP_ADD: {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeInt(a.intVal + b.intVal));
            else if (a.type == BBL::Type::String)
                PUSH(BblValue::makeString(
                    state.intern(asString(a) + toString(b))));
            else
                PUSH(BblValue::makeFloat(toFloat(a) + toFloat(b)));
            break;
        }

        case OP_GET_LOCAL:  PUSH(frame->slots[READ_U16()]); break;
        case OP_SET_LOCAL:  frame->slots[READ_U16()] = PEEK(0); break;

        case OP_JUMP_IF_FALSE: {
            int16_t offset = (int16_t)READ_U16();
            if (isFalsy(PEEK(0))) frame->ip += offset;
            POP();
            break;
        }

        case OP_LOOP: {
            uint16_t offset = READ_U16();
            frame->ip -= offset;
            // Safe point: GC, step limit, termination
            state.gcSafePoint();
            if (++state.stepCount > state.maxSteps && state.maxSteps > 0)
                return INTERPRET_STEP_LIMIT;
            if (state.terminated)
                return INTERPRET_TERMINATED;
            break;
        }

        case OP_CALL: {
            uint8_t argc = READ_BYTE();
            if (!callValue(state, PEEK(argc), argc))
                return INTERPRET_RUNTIME_ERROR;
            frame = &state.vm.frames[state.vm.frameCount - 1];
            // Safe point: step limit, termination
            if (++state.stepCount > state.maxSteps && state.maxSteps > 0)
                return INTERPRET_STEP_LIMIT;
            if (state.terminated)
                return INTERPRET_TERMINATED;
            break;
        }

        case OP_RETURN: {
            BblValue result = POP();
            state.vm.frameCount--;
            if (state.vm.frameCount == 0) {
                POP();  // pop the script function
                return INTERPRET_OK;
            }
            state.vm.stackTop = frame->slots;
            PUSH(result);
            frame = &state.vm.frames[state.vm.frameCount - 1];
            break;
        }

        // ... remaining opcodes follow same pattern ...
        }
    }

    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONST
    #undef PUSH
    #undef POP
    #undef PEEK
}
```

### Computed-goto dispatch (GCC/Clang optimization)

When `__GNUC__` is defined, the switch is replaced with a dispatch table of
label addresses.  Each handler ends by jumping directly to the next, avoiding
the switch comparison overhead.  Measured improvement: 15-30% faster dispatch.

```cpp
#ifdef __GNUC__
    static void* dispatchTable[] = {
        &&op_constant, &&op_null, &&op_true, &&op_false, ...
    };
    #define DISPATCH() goto *dispatchTable[READ_BYTE()]

    DISPATCH();
    op_constant: PUSH(READ_CONST()); DISPATCH();
    op_null:     PUSH(BblValue::makeNull()); DISPATCH();
    // ...
#else
    // fallback to switch
#endif
```

### Closures and captures

BBL's current capture model: at `fn` creation time, `gatherFreeVars()` walks
the AST to find free variables, then copies their current values into a
`vector<pair<uint32_t, BblValue>>` on the `BblFn`.  Value types get a snapshot
copy; GC types share the pointer.

The bytecode design preserves these semantics with a simplified structure:

```cpp
struct BblClosure {
    Chunk* chunk;
    int arity;
    std::vector<BblValue> captures;  // captured values
    bool marked;                     // GC mark
};
```

**Integration with `BblValue`:** a new `BblClosure* closureVal` member is
added to the `BblValue` union.  `BBL::Type::Fn` covers both `BblFn*` (tree-
walker functions, C functions) and `BblClosure*` (bytecode functions).  The
`isCFn` flag distinguishes C functions; a new `isClosure` flag (or checking
`closureVal != nullptr`) distinguishes bytecode closures from tree-walker
functions.  During dual-mode operation, both types coexist.

**GC tracking:** `BblState` gets a new allocation pool
`std::vector<BblClosure*> allocatedClosures`.  The GC sweep processes this
pool alongside the existing `allocatedFns`.  During mark, the collector
traverses each closure's `captures` vector to mark referenced GC objects.

**Why not upvalues?**  The classic clox/Lua upvalue model (open upvalues
pointing into the stack, closed when the frame pops) supports mutable shared
captures -- a closure can mutate a local in an enclosing scope and other
closures see the change.  BBL does not have this semantic.  In BBL, captures
are snapshots at creation time; `=` inside a closure modifies the closure's
own copy, not the original scope.  The simpler capture-by-copy model is both
correct and cheaper (no indirection cell, no close-on-scope-exit logic).

**Compilation of `fn`:**

1. Compiler pushes a new `CompilerState` for the function body.
2. Body is compiled into a new `Chunk`.  Free variables are identified during
   compilation by checking if a symbol resolves in an enclosing compiler scope
   rather than the current one.
3. The compiled chunk is stored as a constant in the enclosing chunk.
4. `OP_CLOSURE <u16 chunkIdx> <u8 captureCount> [<u8 srcType> <u16 srcIdx>]...`
   is emitted.  Each capture descriptor tells the VM where to find the value:
   - `srcType = 0` (LOCAL): read from the enclosing frame's stack slot at `srcIdx`
   - `srcType = 1` (CAPTURE): read from the enclosing closure's captures at `srcIdx`
5. At runtime, `OP_CLOSURE` allocates a `BblClosure`, copies the indicated
   values into `captures`, and pushes it onto the stack.  Maximum 255 captures
   per closure (u8 count).

**Self-capture for recursion:**  When compiling `(= name (fn ...))`, the
compiler detects the pattern and includes `name` in the capture list.  After
the closure is created, `OP_SET_CAPTURE` patches the closure's own slot to
point to itself, enabling direct recursion.

### Tail-call optimization

The current tree-walker implements TCO by throwing a `TailCall` struct that is
caught in `callFn`'s loop, rebinding parameters in-place.

In the bytecode VM, TCO is cleaner:

1. The compiler marks self-recursive calls in tail position (same AST walk as
   `markTailCalls()` today).
2. Tail calls emit `OP_TAIL_CALL <u8 argc>` instead of `OP_CALL`.
3. The VM handler for `OP_TAIL_CALL`:
   a. Slides the new arguments down to overwrite the current frame's slots.
   b. Resets `frame->ip` to the start of the current chunk.
   c. Does NOT push a new `CallFrame`.
   d. Continues the dispatch loop.

No exception overhead, no allocation.  The frame is reused in place.

### GC integration

The VM value stack becomes a GC root.  During the mark phase, the collector
scans `stack[0..stackTop)` and marks every GC-managed value.  Additionally,
all `BblClosure` objects are tracked in the existing allocation pools and
their `captures` vectors are scanned.

**Safe points** remain at the same logical positions: top of `OP_LOOP`
(backward jump), `OP_CALL` entry, and `OP_EXEC`/`OP_EXECFILE`.  The GC never
triggers mid-instruction -- all live values are either on the stack or in
closure captures at safe points.

**Chunk constants** are also GC roots.  A chunk's constant pool may contain
interned strings and closure prototypes that must not be collected while the
chunk is reachable.

### Error handling and backtraces

**Runtime errors** produce `BBL::Error` exactly as today.  The VM constructs
the backtrace by walking `frames[0..frameCount)`, reading each frame's chunk
and `ip` to look up the source line from `chunk->lines`.

**`try`/`catch`** compiles to:

```
OP_TRY_BEGIN  <u16 catchOffset>   // push exception handler
  ... body bytecode ...
OP_TRY_END                        // pop handler, jump past catch
  ... catch bytecode ...          // error message bound to local
```

The VM maintains a small stack of exception handlers (frame index + catch IP).
On error, it unwinds frames to the handler and jumps to the catch block.

### Method dispatch

`ColonAccess` nodes compile to `OP_METHOD_CALL`:

1. Compile the receiver (push object onto stack).
2. Compile each argument (push onto stack).
3. Emit `OP_METHOD_CALL <u16 nameIdx> <u8 argc>`.

The VM handler for `OP_METHOD_CALL` switches on the receiver's type tag and
dispatches to the built-in method tables (same C++ functions as today:
`evalVectorMethod`, `evalStringMethod`, etc).  UserData methods dispatch
through `UserDataDesc::methods` as before.

This is a runtime dispatch -- method names are not resolved at compile time
because BBL is dynamically typed and the receiver type is not known until
execution.

### Dot access

`DotAccess` nodes compile differently based on context:

**Read:** `compile(obj)`, emit `OP_GET_FIELD <u16 nameIdx>`.  Integer-index
dot notation (`v.0`) emits `OP_GET_INDEX` with the index as a constant.

**Write (assignment target):** `compile(value)`, `compile(obj)`, emit
`OP_SET_FIELD <u16 nameIdx>`.

The VM handler for `OP_GET_FIELD` switches on the object type:
- Struct: field read via `FieldDesc` offset (same as today)
- Table: string key lookup
- Other: runtime error

### break and continue

In the tree-walker, `break`/`continue` use a `flowSignal` state flag that is
checked after each loop iteration.  In bytecode, they compile to direct jumps:

- `break` emits `OP_JUMP` targeting the instruction after the loop's
  `OP_LOOP`.  The compiler patches this offset when the loop body finishes
  compiling.
- `continue` emits `OP_LOOP` targeting the loop's condition check.

No runtime flag, no checking overhead.

### Compiler architecture

The compiler is a recursive function operating on `AstNode` trees.  It
maintains a stack of `CompilerState` objects, one per function being compiled
(including the implicit top-level function).

```cpp
struct Local {
    uint32_t symbolId;    // which variable
    int depth;            // scope depth (for block scoping)
};

struct CompilerState {
    Chunk chunk;
    std::vector<Local> locals;
    int scopeDepth = 0;
    CompilerState* enclosing = nullptr;  // for capture resolution

    // Loop tracking for break/continue patching
    struct LoopInfo {
        int start;              // bytecode offset of loop condition
        std::vector<int> breaks; // offsets of break jumps to patch
    };
    std::vector<LoopInfo> loops;
};
```

**Symbol resolution** during compilation:

1. Check `locals` array (innermost scope first) -- emit `OP_GET_LOCAL`.
2. Walk `enclosing` chain to find in an outer function's locals -- emit
   `OP_GET_CAPTURE` and record the capture in the function's capture list.
3. Fall through to global -- emit `OP_GET_GLOBAL`.

This is resolved at compile time, eliminating the runtime scope-chain walk.

### Disassembler

A debug disassembler is included for development.  It prints each instruction
with its offset, opcode name, and decoded operands:

```
0000  OP_CONSTANT       0  (42)
0003  OP_GET_LOCAL       1
0006  OP_ADD
0007  OP_SET_LOCAL       2
000a  OP_LOOP           10  (-> 0000)
```

Enabled by a compile-time flag (`BBL_DEBUG_TRACE`).  When active, the VM
prints each instruction before executing it, plus the current stack contents.

### C function interface

C functions registered via `defn()` continue to work unchanged.  When the VM
encounters `OP_CALL` on a `BblValue` with `isCFn == true`, it:

1. Reads arguments from the value stack into `BblState::callArgs`.
2. Invokes the `BblCFunction` pointer.
3. Pushes the return value (from `BblState::returnValue`) onto the stack.

No changes to the C API.

### Child states

BBL's child-state threading system (`state-new`, `post`, `recv`, `join`, etc.)
creates independent `BblState` instances that run scripts in separate threads.
These work through the C function interface (`defn`), which is preserved.
Each child state's `BblState` independently has its own `VmState` and uses
the bytecode VM.  The `useBytecode` flag propagates to child states at
creation time.  No opcode changes are needed.

---

## Migration strategy

### Dual-mode operation

During migration, `BblState` offers both execution paths:

```cpp
BblValue BblState::execExpr(const std::string& source, BblScope& scope) {
    auto ast = parse(source);
    if (useBytecode) {
        Chunk chunk = compile(ast);
        return vmExecute(chunk);
    } else {
        return eval(ast, scope);
    }
}
```

A runtime flag (`useBytecode`) selects the path.  The full test suite runs in
both modes, and results are compared to ensure semantic equivalence.  The flag
is exposed as a command-line option (`--bytecode` / `--tree-walk`).

### Phase 1 -- Infrastructure

Add `Chunk`, `OpCode` enum, `VmState`, dispatch loop skeleton, and
disassembler.  Compile and execute a trivial subset: integer/float/string
literals, arithmetic (`+` `-` `*` `/` `%`), comparisons, `not`, `null`,
`true`, `false`.

Verify output matches the tree-walker on trivial expressions.  Existing tests
continue running on the tree-walker.

**New files:** `src/chunk.h`, `src/compiler.h`, `src/compiler.cpp`, `src/vm.h`,
`src/vm.cpp`, `src/disasm.h`, `src/disasm.cpp`.

**Estimated size:** ~400 LOC.

### Phase 2 -- Variables and control flow

Add local variables (`=`, `OP_GET_LOCAL`, `OP_SET_LOCAL`), global variables,
`if`, `loop`, `do`, `and`, `or`, `break`, `continue`.  The compiler tracks
locals in `CompilerState` and resolves symbols at compile time.

At this point, `loop_arith` and most arithmetic benchmarks can run on the
bytecode path.

**Estimated size:** ~300 LOC.

### Phase 3 -- Functions and closures

Compile `fn` to `OP_CLOSURE`.  Implement `OP_CALL`, `OP_RETURN`,
`OP_TAIL_CALL`.  Implement capture resolution and `OP_GET_CAPTURE` /
`OP_SET_CAPTURE`.  Handle self-capture for recursive functions.

At this point, `function_calls` benchmark runs on bytecode.

**Estimated size:** ~400 LOC.

### Phase 4 -- Collections and access

Compile `vector`, `table`, `struct`, `binary`, `sizeof`.  Compile dot access
(`OP_GET_FIELD`, `OP_SET_FIELD`) and index access (`OP_GET_INDEX`,
`OP_SET_INDEX`).  Compile colon access (`OP_METHOD_CALL`).

This covers the serialization use case -- the primary purpose of BBL.

**Estimated size:** ~300 LOC.

### Phase 5 -- Remaining features

`try`/`catch` (exception handler stack), `with` (resource cleanup), `each`
(container iteration), `exec`/`execfile` (runtime compilation), `typeof`,
bitwise ops.

**Estimated size:** ~200 LOC.

### Phase 6 -- Switchover

Make bytecode the default.  Keep tree-walker behind `--tree-walk` flag.  Run
full test suite + benchmarks in both modes.  Publish benchmark comparison.
Remove tree-walker in a later release after confidence period.

### Total estimated size

~1600 LOC of new code across compiler, VM, and disassembler.  The existing
~4000 LOC of runtime (GC, value types, string interning, type descriptors, C
API) is unchanged.

---

## Expected performance

| Benchmark               | Tree-walk | Bytecode (est.) | Speedup |
|-------------------------|-----------|-----------------|---------|
| loop_arith (1M)         | 0.065s    | ~0.005-0.013s   | 5-13x   |
| function_calls (500K)   | 0.048s    | ~0.005-0.010s   | 5-10x   |
| gc_pressure (100K)      | 0.010s    | ~0.008-0.010s   | 1-1.3x  |
| string_intern (10K)     | 0.057s    | ~0.055s         | ~1x     |
| table_heavy (1K)        | 0.002s    | ~0.002s         | ~1x     |

Compute-heavy benchmarks (arithmetic, function calls) see the largest gains.
GC-bound and I/O-bound benchmarks see little change because the bottleneck is
not eval dispatch.

---

## Risks

| Risk                            | Likelihood | Impact | Mitigation                        |
|---------------------------------|------------|--------|-----------------------------------|
| Semantic mismatch vs tree-walk  | Medium     | High   | Dual-mode + full test suite       |
| Closure capture semantics drift | Medium     | High   | Dedicated capture test cases      |
| GC misses stack roots           | Low        | High   | Stack is scanned as contiguous array; simpler than scope chain |
| Debug experience degrades       | Medium     | Medium | Line table + disassembler + `--tree-walk` fallback |
| Increased binary size           | Low        | Low    | ~1600 LOC; small relative to existing code |
| Performance regression          | Very Low   | Medium | Bytecode is strictly faster for dispatch-bound code |

---

## References

1. Nystrom, R. *Crafting Interpreters* (2021).
   https://craftinginterpreters.com -- Complete stack-based bytecode VM in C.

2. Ierusalimschy, R., de Figueiredo, L.H., Celes, W. "The Implementation
   of Lua 5.0" (2005).  https://www.lua.org/doc/jucs05.pdf -- Register-based
   VM design with upvalue closures.

3. Nystrom, R. "Game Programming Patterns: Bytecode" (2014).
   https://gameprogrammingpatterns.com/bytecode.html -- Motivation for bytecode
   in game engines.

4. Thegreenplace, E. "Adventures in JIT Compilation" (2017).
   https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-1-an-interpreter/
   -- Interpreter to bytecode to JIT progression.
