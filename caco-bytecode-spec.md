# BBL Bytecode VM -- Investigation

Research into converting BBL from a tree-walking AST interpreter to a bytecode
virtual machine.

---

## 1. Current Architecture

BBL is a dynamically-typed Lisp-style scripting language (s-expression syntax)
used for serializing C++ data structures and binary blobs.  It is currently a
**tree-walking AST interpreter**: every expression goes through `eval()` which
switches on the `AstNode` type, recursively evaluating children.

### Current Performance Profile

| Benchmark                  | Time    | Bottleneck                        |
|----------------------------|---------|-----------------------------------|
| loop_arith (1M iterations) | 0.065s  | eval dispatch ~65ns/iter          |
| function_calls (500K)      | 0.048s  | scope setup + capture ~96ns/call  |
| gc_pressure (100K allocs)  | 0.010s  | sweep + threshold                 |

The implementation.md already notes: *"a bytecode VM would be 5-20x faster but
is out of scope."*  This document investigates making it in-scope.

### Why the Tree-Walker is Slow

1. **Pointer chasing.** AST nodes are heap objects connected by pointers.
   Walking children forces cache misses -- the CPU can't prefetch the next
   instruction because it's at an unpredictable address.

2. **Dispatch overhead.** Every AST node evaluation requires a virtual switch
   through 10+ node types, then a second dispatch through 30+ special forms.
   That's two levels of indirection per expression.

3. **Redundant re-parsing.** Function bodies are stored as AST and re-traversed
   on every call.  Loop bodies re-traverse the same nodes on every iteration.

4. **Object overhead.** Each `AstNode` carries string data, vectors of children,
   mutable caches, etc.  A simple `(+ a 1)` expression is 3 AST nodes with
   pointers, totaling hundreds of bytes for what could be 3-6 bytes of bytecode.

---

## 2. What is Bytecode?

Bytecode is a compact, linear encoding of instructions for a virtual machine.
Instead of walking a tree of objects, the VM reads a flat array of bytes --
each byte (or small group of bytes) is an opcode that tells the VM what to do.

**Key properties:**
- **Dense:** instructions are packed contiguously in memory, ideal for CPU cache
- **Linear:** executed sequentially with explicit jumps for control flow
- **Simple dispatch:** one switch/goto per instruction instead of tree traversal
- **Portable:** runs on any platform that has the VM implementation

Bytecode VMs are the execution model behind Lua, Python, Ruby (YARV), CPython,
Erlang (BEAM), Java (JVM), and .NET (CIL).

---

## 3. Approaches Surveyed

### 3.1 Stack-Based VM (Crafting Interpreters / clox style)

**Source:** Robert Nystrom, *Crafting Interpreters* (2021), chapters 14-30.

The clox approach compiles source to a flat array of single-byte opcodes.
Operands are pushed/popped from an explicit value stack.

**Architecture:**
- Opcodes: 1 byte each (`OP_CONSTANT`, `OP_ADD`, `OP_RETURN`, etc.)
- Operands: inline byte(s) after the opcode (constant index, jump offset)
- Constants: stored in a per-chunk constant pool, referenced by index
- Execution: `for(;;) { switch(*ip++) { ... } }` -- tight dispatch loop
- Values: on a value stack; binary ops pop 2, push 1
- Locals: live on the value stack at known offsets from the frame pointer
- Closures: "upvalues" -- indirection cells that can live on stack or heap
- Functions: each function is a separate `Chunk` (bytecode + constants)

**Pros:**
- Extremely simple to implement.  clox is ~4,500 lines of C.
- Well-documented -- the book walks through every line of code
- Simple calling convention (push args, call, pop result)
- Matches BBL well: dynamically typed, closures, GC

**Cons:**
- Stack-based = more instructions than register-based (more push/pop)
- Slightly slower than register-based VMs in benchmarks (~20-30%)

**Expected speedup over tree-walk:** 5-15x (Nystrom measured ~70x for Lox,
but that included going from Java to C.  Since BBL is already in C++, expect
the lower end of improvement from bytecode alone.)

**Complexity:** Low.  Straightforward single-pass compilation.  Most of the
infrastructure (GC, value types, string interning) already exists in BBL.


### 3.2 Register-Based VM (Lua 5.x style)

**Source:** Lua 5.x implementation; paper "The Implementation of Lua 5.0"
by Roberto Ierusalimschy, Luiz Henrique de Figueiredo, Waldemar Celes (2005).

Lua uses a register-based bytecode where each instruction encodes source and
destination register indices directly.  A `ADD A B C` instruction means
`R[A] = R[B] + R[C]` in a single dispatch step.

**Architecture:**
- Fixed-width 32-bit instructions (opcode + register fields packed into one word)
- Registers map to a contiguous block of the value stack (one block per call frame)
- Fewer instructions executed vs. stack VMs (no explicit push/pop)
- Constants embedded in instructions via a `K` bit (register or constant?)
- Closures use "upvalues" similar to clox but with open/closed distinction

**Pros:**
- Fewer dispatches per operation (one instruction does what 3-4 stack ops do)
- Better data locality (operands are direct array accesses, not stack shuffling)
- Proven at scale -- Lua is the fastest mainstream interpreted language
- Well-suited to BBL's flat slot scope model (slots are effectively registers)

**Cons:**
- More complex instruction encoding (bitfield packing, multiple formats)
- Harder to compile: need register allocation (even a simple linear scan)
- Wider instructions (4 bytes vs 1-2 bytes) -- slightly more memory
- Less documentation for implementors compared to stack-based

**Expected speedup over tree-walk:** 8-20x

**Complexity:** Medium.  Register allocation adds ~200-400 lines of code.
The instruction encoding is more intricate but not fundamentally harder.


### 3.3 Threaded Code / Computed Goto

**Source:** Various; used by CPython, Ruby YARV, gforth.

Not a different VM architecture per se, but a dispatch optimization.  Instead
of a `switch` statement, each opcode handler ends with a `goto *dispatch[*ip++]`
that jumps directly to the next handler, eliminating the switch overhead and
improving branch prediction.

**Architecture:**
- Same as stack or register VM, but the dispatch loop is replaced with computed
  gotos (GCC/Clang `&&label` extension)
- Each opcode handler ends with: `DISPATCH() { goto *dispatch_table[*ip++]; }`
- Reduces branch mispredictions by ~50% on modern CPUs

**Pros:**
- Simple upgrade from a switch-based VM (mechanical transformation)
- 15-30% faster dispatch than switch statements
- Used by CPython, Ruby YARV, Lua (optional)

**Cons:**
- Requires GCC/Clang extension (`__label__`, `&&label`) -- not standard C++
- MSVC does not support computed gotos (would need fallback to switch)
- Marginal improvement on top of a well-optimized switch

**Expected speedup over switch dispatch:** 15-30% (on top of whatever VM
architecture is chosen)

**Complexity:** Very low.  Mechanical change to the dispatch loop.


### 3.4 Direct Threaded / Subroutine Threading

**Source:** Traditional Forth implementations.

Each "instruction" in the bytecode stream is a function pointer (or native code
address) instead of a byte opcode.  The dispatch loop becomes:

```c
while (running) { (*ip++)(); }
```

Or even eliminates the loop entirely by having each handler call the next.

**Pros:**
- Fastest dispatch possible in pure C (one indirect call per instruction)
- No switch, no table lookup

**Cons:**
- Instruction stream is pointers, not bytes -- 8x wider on 64-bit
- Hard to serialize/deserialize
- Poor cache behavior for the instruction stream itself
- Not portable across builds (function addresses change)

**Not recommended for BBL.** The memory cost is too high and serialization
is important for BBL's use case.


### 3.5 NaN-Boxing for Value Representation

**Source:** Used by LuaJIT, SpiderMonkey, JavaScriptCore.

A technique for encoding multiple value types into a single 64-bit word by
exploiting the structure of IEEE 754 NaN values.  All quiet NaN values have
the same bits in the exponent and mantissa, leaving ~50 bits for payload.

```
double:   normal IEEE 754 value
int:      NaN bits | 0x0002000000000000 | (int32 in lower 32 bits)
pointer:  NaN bits | 0x0001000000000000 | (48-bit pointer)
bool:     NaN bits | 0x0004000000000000 | (0 or 1)
nil:      NaN bits | 0x0003000000000000
```

**Pros:**
- 8 bytes per value regardless of type (no tag word, no union padding)
- Floating point values are used directly -- no unboxing needed
- Type checking is a bitmask test (very fast)

**Cons:**
- Tricky to implement correctly (endianness, pointer width assumptions)
- Only 32-bit integers fit natively; 64-bit ints need boxing or truncation
- BBL uses 64-bit signed integers -- **this is a problem**

**Assessment for BBL:** Not ideal.  BBL's 64-bit integer type would need to
be boxed (heap-allocated) under NaN-boxing, losing its performance advantage.
BBL's current tagged union (`BblValue`) with a type enum + union of int64/
double/pointer is already compact (16 bytes) and well-optimized.  The savings
from NaN-boxing (16 -> 8 bytes) are modest and the 64-bit int limitation is
a dealbreaker.

**Not recommended** unless BBL drops to 32-bit integers.


### 3.6 JIT Compilation (LLVM / hand-rolled)

**Source:** LuaJIT (Mike Pall), V8, various.

Compile bytecode (or source) to native machine code at runtime.

**Pros:**
- 10-100x faster than interpretation
- Can specialize for observed types

**Cons:**
- Enormous implementation complexity (10,000+ lines minimum for a basic JIT)
- Platform-specific (x86_64, ARM, etc.)
- Compilation latency at startup
- Debugging is extremely difficult
- Completely overkill for BBL's use case (game asset serialization scripts)

**Not recommended.** The cost/benefit ratio is terrible for BBL.  A bytecode
interpreter will be more than fast enough.

---

## 4. Recommendation

### Stack-based bytecode VM with computed-goto dispatch

This is the clear winner for BBL:

| Criterion          | Stack VM | Register VM | JIT    |
|--------------------|----------|-------------|--------|
| Implementation     | ~1500 LOC| ~2500 LOC   | 10K+   |
| Expected speedup   | 5-15x   | 8-20x       | 50-100x|
| Complexity         | Low      | Medium      | Extreme|
| Debugging ease     | High     | Medium      | Low    |
| Fits existing code | High     | Medium      | Low    |

**Rationale:**

1. **Simplest path.**  BBL already has all the runtime infrastructure (GC,
   value types, string interning, scope management).  The only new code is the
   compiler (AST -> bytecode) and the dispatch loop.  A stack VM compiler is
   essentially a post-order AST traversal that emits push/op/pop sequences.

2. **BBL's slot model maps naturally.**  Function scopes already use flat
   `vector<BblValue>` slots.  In a stack VM, these become stack-frame locals
   at known offsets.  No register allocator needed.

3. **Closures are solved.**  The clox upvalue model (Lua-derived) handles
   exactly the capture semantics BBL uses: value snapshot for primitives,
   shared reference for GC types.

4. **Computed goto is a free bonus.**  Since BBL targets GCC/Linux (primary
   platform), computed gotos are available and provide 15-30% on top.  A
   switch fallback can be `#ifdef`-ed for MSVC.

5. **Good enough performance.**  A 5-15x speedup takes `loop_arith` from
   65ns/iteration to ~5-13ns/iteration.  That's in the range of Lua and
   CPython, which is more than sufficient for a serialization scripting
   language.

6. **Path to register VM later.**  If more speed is ever needed, the bytecode
   format can be evolved to register-based without changing the rest of the
   system.  But this is unlikely to be necessary.

---

## 5. Proposed Design Sketch

### 5.1 Instruction Set

One-byte opcodes.  Multi-byte operands where needed (constant index, jump
offset, local slot index).  Target: ~40-50 opcodes total.

```
-- Constants & Literals --
OP_CONST_I64    <u16 idx>       push constants[idx] (int64)
OP_CONST_F64    <u16 idx>       push constants[idx] (float64)
OP_CONST_STR    <u16 idx>       push constants[idx] (interned string)
OP_NULL                         push null
OP_TRUE                         push true
OP_FALSE                        push false

-- Arithmetic --
OP_ADD                          pop b, pop a, push a + b
OP_SUB                          pop b, pop a, push a - b
OP_MUL                          pop b, pop a, push a * b
OP_DIV                          pop b, pop a, push a / b
OP_MOD                          pop b, pop a, push a % b
OP_NEG                          pop a, push -a

-- Bitwise --
OP_BAND                         pop b, pop a, push a & b
OP_BOR                          pop b, pop a, push a | b
OP_BXOR                         pop b, pop a, push a ^ b
OP_BNOT                         pop a, push ~a
OP_SHL                          pop b, pop a, push a << b
OP_SHR                          pop b, pop a, push a >> b

-- Comparison --
OP_EQ                           pop b, pop a, push a == b
OP_NEQ                          pop b, pop a, push a != b
OP_LT                           pop b, pop a, push a < b
OP_GT                           pop b, pop a, push a > b
OP_LTE                          pop b, pop a, push a <= b
OP_GTE                          pop b, pop a, push a >= b

-- Logic --
OP_NOT                          pop a, push !a

-- Variables --
OP_GET_LOCAL    <u16 slot>      push frame.locals[slot]
OP_SET_LOCAL    <u16 slot>      frame.locals[slot] = peek top
OP_GET_UPVALUE <u8 idx>        push closure.upvalues[idx]
OP_SET_UPVALUE <u8 idx>        closure.upvalues[idx] = peek top
OP_GET_GLOBAL  <u16 idx>       push globals[constants[idx]]
OP_SET_GLOBAL  <u16 idx>       globals[constants[idx]] = peek top

-- Control Flow --
OP_JUMP        <i16 offset>    ip += offset
OP_JUMP_IF_FALSE <i16 offset>  pop, if falsy: ip += offset
OP_JUMP_IF_TRUE  <i16 offset>  pop, if truthy: ip += offset
OP_LOOP        <u16 offset>    ip -= offset (backward jump)

-- Functions & Calls --
OP_CALL        <u8 argc>       call value at stack[top - argc - 1]
OP_CLOSURE     <u16 idx> ...   create closure from constants[idx]
OP_RETURN                      return top of stack to caller
OP_TAIL_CALL   <u8 argc>       tail-call optimization

-- Collections --
OP_VECTOR      <u16 count>     pop count values, push new vector
OP_TABLE       <u16 count>     pop count*2 values (k,v pairs), push table

-- Access --
OP_GET_FIELD   <u16 idx>       pop obj, push obj.field (field name = const[idx])
OP_SET_FIELD   <u16 idx>       pop val, pop obj, obj.field = val
OP_GET_INDEX                   pop idx, pop obj, push obj[idx]
OP_SET_INDEX                   pop val, pop idx, pop obj, obj[idx] = val
OP_METHOD_CALL <u16 idx> <u8 argc>  obj:method(args)

-- Misc --
OP_POP                         discard top of stack
OP_DUP                         duplicate top of stack
OP_PRINT                       pop and print (for debug / `print` special form)
```

### 5.2 Compilation Model

The compiler is a single-pass recursive descent over the existing AST.
No new parser needed -- the existing parser produces `AstNode` trees, and a
new `compile()` function walks them to emit bytecode.

```
compile(AstNode) -> Chunk {
    switch (node.type) {
        IntLiteral:    emit OP_CONST_I64, addConstant(node.intVal)
        FloatLiteral:  emit OP_CONST_F64, addConstant(node.floatVal)
        StringLiteral: emit OP_CONST_STR, addConstant(node.strVal)
        Symbol:        emit OP_GET_LOCAL/OP_GET_UPVALUE/OP_GET_GLOBAL
        List:          compileCall(node)   // dispatch special forms vs calls
        DotAccess:     compile(obj), emit OP_GET_FIELD
        ColonAccess:   compile(obj), compile(args), emit OP_METHOD_CALL
    }
}

compileCall(node) {
    op = node.children[0]
    if (isSpecialForm(op)):
        switch(op):
            "+":  compile(lhs), compile(rhs), emit OP_ADD
            "if": compile(cond), emit OP_JUMP_IF_FALSE, compile(then),
                  emit OP_JUMP, patch(else), compile(else), patch(end)
            "loop": loopStart, compile(cond), emit OP_JUMP_IF_FALSE,
                    compile(body), emit OP_LOOP, patch(exit)
            "fn":   compileFunctionBody(node)
            ...
    else:
        // Regular function call
        compile(callee)
        for each arg: compile(arg)
        emit OP_CALL, argc
}
```

### 5.3 Data Structures

```cpp
struct Chunk {
    std::vector<uint8_t> code;       // bytecode
    std::vector<BblValue> constants; // constant pool
    std::vector<int> lines;          // source line per instruction (debug)
};

struct CallFrame {
    Chunk* chunk;              // bytecode being executed
    uint8_t* ip;               // instruction pointer
    BblValue* slots;           // pointer into value stack (frame base)
    ObjClosure* closure;       // closure for upvalue access
};

struct VM {
    CallFrame frames[MAX_FRAMES];   // call stack
    int frameCount;
    BblValue stack[MAX_STACK];      // value stack
    BblValue* stackTop;
    // ... existing BblState members (GC, interns, etc.)
};
```

### 5.4 Dispatch Loop

```cpp
InterpretResult run() {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];

    #define READ_BYTE()    (*frame->ip++)
    #define READ_U16()     (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_CONST()   (frame->chunk->constants[READ_U16()])
    #define PUSH(val)      (*vm.stackTop++ = (val))
    #define POP()          (*--vm.stackTop)
    #define PEEK(n)        (vm.stackTop[-1 - (n)])

    for (;;) {
        uint8_t instruction = READ_BYTE();
        switch (instruction) {

        case OP_CONST_I64: PUSH(READ_CONST()); break;

        case OP_ADD: {
            BblValue b = POP(), a = POP();
            if (isInt(a) && isInt(b))
                PUSH(intVal(asInt(a) + asInt(b)));
            else
                PUSH(floatVal(asFloat(a) + asFloat(b)));
            break;
        }

        case OP_GET_LOCAL: PUSH(frame->slots[READ_U16()]); break;
        case OP_SET_LOCAL: frame->slots[READ_U16()] = PEEK(0); break;

        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_U16();
            if (isFalsy(PEEK(0))) frame->ip += offset;
            POP();
            break;
        }

        case OP_CALL: {
            uint8_t argc = READ_BYTE();
            callValue(PEEK(argc), argc);
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }

        case OP_RETURN: {
            BblValue result = POP();
            vm.frameCount--;
            if (vm.frameCount == 0) return INTERPRET_OK;
            vm.stackTop = frame->slots;
            PUSH(result);
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }

        // ... remaining opcodes ...
        }
    }
}
```

### 5.5 Closure / Upvalue Design

Following the Lua/clox model:

```cpp
struct Upvalue {
    BblValue* location;  // points into stack (open) or to 'closed' field
    BblValue closed;     // value after closing
    Upvalue* next;       // linked list of open upvalues (for closing)
};

struct ObjClosure {
    Chunk* chunk;
    std::vector<Upvalue*> upvalues;
};
```

- **Open upvalue:** points to a live stack slot.  Multiple closures can share
  the same upvalue if they capture the same variable.
- **Closing:** when a local goes out of scope, its upvalue's `location` is
  redirected to `&closed` and the value is copied there.  The closure can now
  outlive the stack frame.
- **BBL adaptation:** BBL captures by value for primitives and by reference
  for GC types.  This maps cleanly: primitive captures become closed upvalues
  at creation time; GC-type captures remain open (shared).

---

## 6. Migration Strategy

### Phase 1: Bytecode infrastructure (no behavior change)

Add `Chunk`, `VM`, opcode enum, and dispatch loop.  Compile a trivial subset
(integer literals, arithmetic, print) and verify output matches the
tree-walker.  Run existing tests -- they still use the tree-walker.

### Phase 2: Compile core expressions

Add compilation for: variables (local/global), assignment, `if`, `loop`,
`and`/`or`, comparison, function definition and calls.  At this point, most
benchmarks can run on the bytecode path.

### Phase 3: Closures and upvalues

Implement the upvalue model.  Compile `fn` with captures.  Verify closure
semantics match the tree-walker (value vs reference capture, self-capture).

### Phase 4: Collections and access

Compile vector/table construction, dot access, colon access (method calls),
index operations.  This covers struct field access and the serialization
use case.

### Phase 5: Remaining features

`try`/`catch`, `with`, `exec`/`execfile`, `each`, `break`/`continue`,
`typeof`, `fmt`, and any remaining special forms.

### Phase 6: Switchover and cleanup

Make bytecode the default path.  Keep the tree-walker behind a flag for
debugging.  Remove it in a later release once confidence is high.

### Dual-mode operation

During migration, `BblState` can offer both paths:

```cpp
BblValue BblState::run(const std::string& source) {
    auto ast = parse(source);
    if (useBytecode) {
        Chunk chunk = compile(ast);
        return vmExecute(chunk);
    } else {
        return eval(ast, rootScope);
    }
}
```

This allows incremental migration and A/B testing of results.

---

## 7. Risk Assessment

| Risk                              | Likelihood | Mitigation                         |
|-----------------------------------|------------|------------------------------------|
| Semantic mismatch (bytecode vs AST)| Medium    | Dual-mode + full test suite        |
| Closure capture semantics differ  | Medium     | Dedicated test cases for captures  |
| GC interaction with value stack   | Low        | Stack is a GC root; same as scopes |
| Performance regression            | Very Low   | Bytecode is strictly faster        |
| Increased binary size             | Low        | ~1500 LOC; compiler is small       |
| Debug experience degrades         | Medium     | Line number table + disassembler   |

---

## 8. References

1. Nystrom, R. *Crafting Interpreters* (2021).
   https://craftinginterpreters.com/chunks-of-bytecode.html
   -- Complete walkthrough of a stack-based bytecode VM in C.

2. Ierusalimschy, R., de Figueiredo, L.H., Celes, W. "The Implementation
   of Lua 5.0" (2005).  https://www.lua.org/doc/jucs05.pdf
   -- Register-based VM design with upvalue closures.

3. Nystrom, R. "Game Programming Patterns: Bytecode" (2014).
   https://gameprogrammingpatterns.com/bytecode.html
   -- Practical motivation for bytecode in game engines.

4. Thegreenplace, E. "Adventures in JIT Compilation" (2017).
   https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-1-an-interpreter/
   -- Progression from interpreter to optimized bytecode to JIT.

5. Bernstein, M. "Compiling a Lisp" (2020).
   https://bernsteinbear.com/blog/compiling-a-lisp-2/
   -- Compiling Lisp s-expressions to native code; pointer tagging.

6. Wikipedia. "Bytecode."
   https://en.wikipedia.org/wiki/Bytecode
   -- Survey of bytecode formats and VMs across languages.
