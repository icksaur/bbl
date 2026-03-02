# Register-Based Bytecode VM
Status: proposed

## Goal

Replace the stack-based bytecode VM with a register-based VM using 32-bit
fixed-width instructions.  Each instruction encodes source and destination
register indices directly, eliminating push/pop overhead and reducing the
dispatch count per operation by ~2.5x.  The parser and existing runtime
(GC, string interning, type descriptors, C API) are unchanged.

## Design

### Instruction format

All instructions are 32 bits wide, packed into a single `uint32_t`.
Three formats modeled after Lua 5.x:

```
iABC:   op(8) | A(8) | B(8) | C(8)       three registers
iABx:   op(8) | A(8) | Bx(16)            register + unsigned 16-bit
iAsBx:  op(8) | A(8) | sBx(16)           register + signed 16-bit
```

- `op`: 8-bit opcode (supports up to 256 opcodes, currently ~50)
- `A`, `B`, `C`: 8-bit register indices (0-255 per frame)
- `Bx`: 16-bit unsigned (constant index, capture index, argument count)
- `sBx`: 16-bit signed (jump offset)

Registers are a contiguous array of `BblValue` in each call frame.
Register 0 is the callee, registers 1..arity are parameters, and the
compiler allocates temporaries above that.

### Register file

Each `CallFrame` holds a pointer into the value stack, identical to the
current `frame->slots`.  Register R[i] is `frame->regs[i]`.  The stack
grows per-frame by the number of registers the function needs (known at
compile time, stored in the chunk metadata).

```cpp
struct Chunk {
    std::vector<uint32_t> code;      // 32-bit instructions
    std::vector<BblValue> constants;
    std::vector<int> lines;          // one per instruction (not per byte)
    uint8_t numRegs;                 // registers needed by this function
};
```

### Instruction set

```
-- Constants --
LOADK     A Bx       R[A] = constants[Bx]
LOADNULL  A          R[A] = null
LOADBOOL  A B        R[A] = (B != 0)
LOADINT   A sBx      R[A] = sBx  (small integer, -32768..32767)

-- Arithmetic --
ADD       A B C      R[A] = R[B] + R[C]
ADDK      A B C      R[A] = R[B] + constants[C]
SUB       A B C      R[A] = R[B] - R[C]
MUL       A B C      R[A] = R[B] * R[C]
DIV       A B C      R[A] = R[B] / R[C]
MOD       A B C      R[A] = R[B] % R[C]

-- Bitwise --
BAND      A B C      R[A] = R[B] & R[C]
BOR       A B C      R[A] = R[B] | R[C]
BXOR      A B C      R[A] = R[B] ^ R[C]
BNOT      A B        R[A] = ~R[B]
SHL       A B C      R[A] = R[B] << R[C]
SHR       A B C      R[A] = R[B] >> R[C]

-- Comparison (sets R[A] to bool) --
EQ        A B C      R[A] = (R[B] == R[C])
NEQ       A B C      R[A] = (R[B] != R[C])
LT        A B C      R[A] = (R[B] < R[C])
GT        A B C      R[A] = (R[B] > R[C])
LTE       A B C      R[A] = (R[B] <= R[C])
GTE       A B C      R[A] = (R[B] >= R[C])

-- Logic --
NOT       A B        R[A] = !R[B]

-- Control flow --
JMP       sBx        ip += sBx
JMPFALSE  A sBx      if falsy(R[A]): ip += sBx
JMPTRUE   A sBx      if truthy(R[A]): ip += sBx
LOOP      sBx        ip -= sBx  (backward jump + GC safe point)

-- Variables --
GETGLOBAL A Bx       R[A] = globals[constants[Bx]]
SETGLOBAL A Bx       globals[constants[Bx]] = R[A]
GETCAPTURE A B       R[A] = closure.captures[B]
SETCAPTURE A B       closure.captures[B] = R[A]
MOVE      A B        R[A] = R[B]

-- Functions --
CALL      A B C      call R[A] with B args, C results (args in R[A+1..A+B])
TAILCALL  A B        tail-call R[A] with B args (args in R[A+1..A+B])
RETURN    A          return R[A]
CLOSURE   A Bx       R[A] = new closure from constants[Bx], captures follow

-- Short-circuit --
AND       A sBx      if falsy(R[A]): ip += sBx  (leave R[A] as result)
OR        A sBx      if truthy(R[A]): ip += sBx (leave R[A] as result)

-- Collections --
VECTOR    A Bx C     R[A] = new vector, type=constants[Bx], C elements from R[A+1..A+C]
TABLE     A B        R[A] = new table from B key-value pairs at R[A+1..A+2B]
STRUCT    A Bx C     R[A] = construct struct type constants[Bx], C args from R[A+1..A+C]
BINARY    A B        R[A] = binary from R[B]
LENGTH    A B        R[A] = length of R[B]
SIZEOF    A Bx       R[A] = sizeof struct type constants[Bx]

-- Field/Index --
GETFIELD  A B Bx     R[A] = R[B].constants[Bx]  (field name in const pool)
SETFIELD  A B Bx     R[B].constants[Bx] = R[A]
GETINDEX  A B C      R[A] = R[B][R[C]]
SETINDEX  A B C      R[B][R[C]] = R[A]
MCALL     A B Bx     R[A]:method(B args at R[A+1..A+B]), name=constants[Bx], result in R[A]

-- Exception --
TRYBEGIN  A sBx      push handler (catch at ip+sBx, error into R[A])
TRYEND               pop handler

-- Misc --
EXEC      A B        R[A] = exec(R[B])   (string -> eval)
EXECFILE  A B        execfile(R[B])
```

### Key differences from stack VM

| Aspect | Stack VM | Register VM |
|--------|----------|-------------|
| `(+ sum i)` | GET_LOCAL, GET_LOCAL, ADD (3 dispatches) | ADD A B C (1 dispatch) |
| `(= sum (+ sum i))` | GET_LOCAL, GET_LOCAL, ADD, SET_LOCAL, POP (5) | ADD A B C (1, writes directly to sum's register) |
| `(< i n)` | GET_LOCAL, GET_LOCAL, LT (3) | LT A B C (1) |
| Loop iteration total | 13 dispatches | ~5 dispatches |
| Instruction width | 1-3 bytes variable | 4 bytes fixed |
| Code size | ~35 bytes/loop | ~20 bytes/loop |

### Compiler changes

The compiler needs a **register allocator**.  For BBL's simple structure,
a linear-scan allocator suffices:

```cpp
struct RegAllocator {
    uint8_t nextReg = 0;    // next free register
    uint8_t maxRegs = 0;    // high-water mark

    uint8_t alloc() { maxRegs = std::max(maxRegs, uint8_t(nextReg+1)); return nextReg++; }
    void free(uint8_t r) { if (r == nextReg - 1) nextReg--; }
    void freeFrom(uint8_t r) { nextReg = r; }
};
```

Each `compileNode` call takes a `dest` register parameter — the register
to write the result into.  For `(+ a b)`:

```
compileNode(node.children[1], regB)  → emit LOADK/MOVE to put 'a' into regB
compileNode(node.children[2], regC)  → emit LOADK/MOVE to put 'b' into regC
emit ADD(dest, regB, regC)
```

When the source is already in a known register (a local variable), no
MOVE is needed — the compiler passes that register directly as B or C.

Variable-to-register mapping:
- Function parameters: R[1] .. R[arity]
- Local variables: next available register, allocated on first `=`
- Captures: loaded into a temp register via GETCAPTURE when needed
- Globals: loaded via GETGLOBAL into a temp register
- Constants: loaded via LOADK/LOADINT, or used inline via ADDK

### ADDK optimization

For the extremely common `(= i (+ i 1))` pattern, the compiler can emit
`ADDK A A C` where C is a constant index for the integer 1.  This avoids
loading the constant into a register.  The VM handler:

```cpp
case OP_ADDK: {
    BblValue& a = R(A); BblValue& b = R(B); BblValue& k = K(C);
    if (a.type == Int && k.type == Int) R(A).intVal = b.intVal + k.intVal;
    else ...
}
```

### Dispatch loop

```cpp
for (;;) {
    uint32_t inst = *frame->ip++;
    uint8_t op = inst & 0xFF;
    uint8_t A  = (inst >> 8) & 0xFF;
    uint8_t B  = (inst >> 16) & 0xFF;
    uint8_t C  = (inst >> 24) & 0xFF;
    uint16_t Bx = (inst >> 16) & 0xFFFF;
    int16_t sBx = (int16_t)Bx;

    switch (op) {
    case OP_ADD: {
        BblValue& rb = frame->regs[B];
        BblValue& rc = frame->regs[C];
        if (rb.type == Int && rc.type == Int)
            frame->regs[A] = BblValue::makeInt(rb.intVal + rc.intVal);
        else ...
        DISPATCH();
    }
    case OP_LOADK:
        frame->regs[A] = frame->chunk->constants[Bx];
        DISPATCH();
    case OP_JMPFALSE:
        if (isFalsy(frame->regs[A])) frame->ip += sBx;
        DISPATCH();
    ...
    }
}
```

Each instruction is decoded from a single `uint32_t` read.  No
variable-length decoding, no READ_BYTE/READ_U16 macros.  The computed
goto table works identically (index by `op`).

### Loop example: `loop_arith` hot loop

BBL source:
```lisp
(loop (< i n)
    (= sum (+ sum i))
    (= i (+ i 1)))
```

Register assignment: R1=n, R2=sum, R3=i (parameters/locals of enclosing fn).

Register bytecode:
```
0: LT      R4, R3, R1       ; R4 = (i < n)
1: JMPFALSE R4, +4           ; exit if false
2: ADD      R2, R2, R3       ; sum = sum + i
3: ADDK     R3, R3, K0       ; i = i + 1  (K0 = constant 1)
4: LOOP     -5                ; jump back to instruction 0
5: ...                        ; exit
```

**5 instructions per iteration** vs 13 in the stack VM.  2.6x fewer
dispatches.  With computed goto at ~2 cycles/dispatch, that's ~10 cycles
of dispatch vs ~26 — matching Lua 5.4's instruction count exactly.

### Migration strategy

The register VM replaces the stack VM (compiler.cpp, vm.cpp, chunk.h).
The files are rewritten, not patched — the stack VM code is deleted.

1. Rewrite `chunk.h`: 32-bit instruction format, new OpCode enum.
2. Rewrite `compiler.cpp`: register allocator + new code generation.
3. Rewrite `vm.cpp`: new dispatch loop reading 32-bit instructions.
4. Update `disasm.cpp`: decode 32-bit instructions.
5. Keep `vm.h` structures (BblClosure, CallFrame, VmState) with minimal
   changes (ip becomes `uint32_t*`, add `numRegs` to Chunk).

Existing tests continue to pass — the bytecode format is internal;
the API (`exec`, `execExpr`, `--bytecode` flag) is unchanged.

## Considerations

**Why not keep the stack VM alongside?**  Nobody uses BBL.  The stack VM
was a stepping stone.  Maintaining two VMs doubles the bug surface for
no benefit.

**Why 32-bit fixed-width?**  Variable-width register instructions (like
some VMs use) save code size but complicate decoding.  Fixed-width means
one `uint32_t` fetch per instruction, which is optimal for modern CPUs
with 64-bit memory buses.  Lua 5.x uses the same approach.

**Why 8-bit register indices?**  256 registers per frame is more than
enough.  BBL functions are small (typically < 20 locals).  The 8-bit
field fits naturally in the 32-bit instruction word.

**ADDK vs separate LOADK+ADD?**  The `(= i (+ i 1))` pattern occurs in
every loop.  Without ADDK, it requires LOADK+ADD = 2 dispatches.  With
ADDK, it's 1 dispatch.  This single optimization saves ~15% on
loop-heavy benchmarks.  Lua has the same opcode (ADDI).

**GC safety:**  The register file IS the value stack (same memory).  GC
already scans `stack[0..stackTop)`.  No changes needed.

**Closure captures:**  Same as current — capture-by-copy at creation
time.  GETCAPTURE loads a capture into a temp register.  SETCAPTURE
writes back.  No upvalue indirection.

**C function calls:**  Same as current.  CALL detects `isCFn`, copies
args from registers into `state.callArgs`, calls the C function, puts
the result in R[A].

**Risk: correctness.**  The compiler is a full rewrite.  Every special
form must be reimplemented with register allocation.  Mitigation: the
existing 780 tests cover all language features.  Run them after each
phase.

## Acceptance

- All 780 existing tests pass.
- `loop_arith` (function-wrapped) runs within 1.5x of Lua 5.4.
- `function_calls` benchmark matches or beats Lua 5.4.
- Disassembler shows register instructions for all compiled code.
- `--bytecode` flag works identically to current behavior.
