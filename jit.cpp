#include "jit.h"
#include "bbl.h"
#include "vm.h"
#include <cstring>
#include <vector>
#include <sys/mman.h>

// BblValue layout: type at +0 (4 bytes), intVal at +8 (8 bytes), sizeof=16
// Register R[i] at byte offset i*16 from rbx

static constexpr int VAL_SIZE = 16;
static constexpr int TYPE_OFF = 0;
static constexpr int DATA_OFF = 8;
static constexpr int TYPE_INT = 2;
static constexpr int TYPE_NULL = 0;
static constexpr int TYPE_BOOL = 1;

// C helper functions callable from JIT'd code
extern "C" {
    void jitGetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg);
    void jitSetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg);
    void jitCall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc);
    void jitGetCapture(BblValue* regs, BblState* state, uint8_t destReg, uint8_t capIdx);
    void jitSetCapture(BblValue* regs, BblState* state, uint8_t srcReg, uint8_t capIdx);
    void jitClosure(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint16_t protoIdx);
}

void jitGetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg) {
    auto it = state->vm->globals.find(symId);
    if (it != state->vm->globals.end()) { regs[destReg] = it->second; return; }
    auto* val = state->rootScope.lookup(symId);
    if (val) { regs[destReg] = *val; return; }
    throw BBL::Error{"undefined variable"};
}

void jitSetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg) {
    state->vm->globals[symId] = regs[srcReg];
    state->rootScope.def(symId, regs[srcReg]);
}

void jitCall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc) {
    BblValue callee = regs[base];
    if (callee.type == BBL::Type::Fn && callee.isCFn) {
        state->callArgs.clear();
        for (int i = 0; i < argc; i++)
            state->callArgs.push_back(regs[base + 1 + i]);
        state->hasReturn = false;
        state->returnValue = BblValue::makeNull();
        callee.cfnVal(state);
        regs[base] = state->hasReturn ? state->returnValue : BblValue::makeNull();
    } else if (callee.type == BBL::Type::Fn && callee.isClosure) {
        BblClosure* closure = callee.closureVal;
        std::vector<BblValue> calleeRegs(closure->chunk.numRegs);
        calleeRegs[0] = callee;
        for (int i = 0; i < argc; i++)
            calleeRegs[1 + i] = regs[base + 1 + i];

        if (!closure->jitCache) {
            JitCode* cached = new JitCode(jitCompile(*state, closure->chunk));
            closure->jitCache = cached;
        }
        typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
        JitFn fn = reinterpret_cast<JitFn>(closure->jitCache->buf);
        regs[base] = fn(calleeRegs.data(), state, &closure->chunk);
    } else if (callee.type == BBL::Type::Fn) {
        BblFn* fn = callee.fnVal;
        regs[base] = state->callFn(fn, &regs[base + 1], argc, 0);
    } else {
        throw BBL::Error{"not callable"};
    }
}

void jitGetCapture(BblValue* regs, BblState* state, uint8_t destReg, uint8_t capIdx) {
    (void)state;
    // Captures need the closure pointer — for now, not supported in JIT
    throw BBL::Error{"JIT: captures not yet supported"};
}

void jitSetCapture(BblValue* regs, BblState* state, uint8_t srcReg, uint8_t capIdx) {
    (void)state;
    throw BBL::Error{"JIT: captures not yet supported"};
}

void jitClosure(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint16_t protoIdx) {
    BblClosure* proto = chunk->constants[protoIdx].closureVal;
    BblClosure* closure = new BblClosure();
    closure->chunk = proto->chunk;
    closure->arity = proto->arity;
    closure->name = proto->name;
    closure->captureDescs = proto->captureDescs;
    closure->captures.resize(proto->captureDescs.size());
    state->allocatedClosures.push_back(closure);
    // No capture filling in JIT context (top-level closures have no captures)
    regs[destReg] = BblValue::makeClosure(closure);
}

static void emit(uint8_t* buf, size_t& pos, const void* data, size_t len) {
    std::memcpy(buf + pos, data, len);
    pos += len;
}

static void emit8(uint8_t* buf, size_t& pos, uint8_t v) { buf[pos++] = v; }

static void emit32(uint8_t* buf, size_t& pos, uint32_t v) {
    std::memcpy(buf + pos, &v, 4);
    pos += 4;
}

static void emit64(uint8_t* buf, size_t& pos, uint64_t v) {
    std::memcpy(buf + pos, &v, 8);
    pos += 8;
}

static void patchRel32(uint8_t* buf, size_t patchOffset, size_t target) {
    int32_t rel = static_cast<int32_t>(target - (patchOffset + 4));
    std::memcpy(buf + patchOffset, &rel, 4);
}

// Prologue: push rbx, r12, r13; mov rbx,rdi; mov r12,rsi; mov r13,rdx
static void emitPrologue(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x53,                   // push rbx
        0x41, 0x54,             // push r12
        0x41, 0x55,             // push r13
        0x48, 0x89, 0xfb,       // mov rbx, rdi  (regs)
        0x49, 0x89, 0xf4,       // mov r12, rsi  (state)
        0x49, 0x89, 0xd5,       // mov r13, rdx  (chunk)
    };
    emit(buf, pos, code, sizeof(code));
}

// Epilogue: pop r13, r12, rbx; ret
static void emitEpilogue(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x41, 0x5d,             // pop r13
        0x41, 0x5c,             // pop r12
        0x5b,                   // pop rbx
        0xc3,                   // ret
    };
    emit(buf, pos, code, sizeof(code));
}

// RETURN R[A]: load result into rax (intVal) and edx (type), then epilogue
// Caller will interpret rax as the return value
// We return a BblValue by value — on System V ABI, a 16-byte struct
// is returned in rax (first 8 bytes) and rdx (second 8 bytes)
static void emitReturn(uint8_t* buf, size_t& pos, int A) {
    int aOff = A * VAL_SIZE;
    // System V ABI: 16-byte struct returned in rax (bytes 0-7) + rdx (bytes 8-15)
    // BblValue: bytes 0-7 = type+flags, bytes 8-15 = intVal/union
    // mov rax, [rbx + aOff]       — first 8 bytes
    uint8_t load0[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load0, 3);
    emit32(buf, pos, aOff);
    // mov rdx, [rbx + aOff + 8]   — second 8 bytes
    uint8_t load1[] = { 0x48, 0x8b, 0x93 };
    emit(buf, pos, load1, 3);
    emit32(buf, pos, aOff + 8);

    emitEpilogue(buf, pos);
}

// ADD R[A] = R[B] + R[C] (integer only, no type guard for now)
static void emitAdd(uint8_t* buf, size_t& pos, int A, int B, int C) {
    // mov rax, [rbx + B*16+8]
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE + DATA_OFF);
    // add rax, [rbx + C*16+8]
    uint8_t add1[] = { 0x48, 0x03, 0x83 };
    emit(buf, pos, add1, 3);
    emit32(buf, pos, C * VAL_SIZE + DATA_OFF);
    // mov [rbx + A*16+8], rax
    uint8_t mov2[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    // mov dword [rbx + A*16], TYPE_INT
    uint8_t setType[] = { 0xc7, 0x83 };
    emit(buf, pos, setType, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit32(buf, pos, TYPE_INT);
}

// SUB R[A] = R[B] - R[C]
static void emitSub(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE + DATA_OFF);
    // sub rax, [rbx + C*16+8]
    uint8_t sub1[] = { 0x48, 0x2b, 0x83 };
    emit(buf, pos, sub1, 3);
    emit32(buf, pos, C * VAL_SIZE + DATA_OFF);
    uint8_t mov2[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    uint8_t setType[] = { 0xc7, 0x83 };
    emit(buf, pos, setType, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit32(buf, pos, TYPE_INT);
}

// MUL R[A] = R[B] * R[C]
static void emitMul(uint8_t* buf, size_t& pos, int A, int B, int C) {
    // mov rax, [rbx + B*16+8]
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE + DATA_OFF);
    // imul rax, [rbx + C*16+8]
    uint8_t imul[] = { 0x48, 0x0f, 0xaf, 0x83 };
    emit(buf, pos, imul, 4);
    emit32(buf, pos, C * VAL_SIZE + DATA_OFF);
    // store
    uint8_t mov2[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    uint8_t setType[] = { 0xc7, 0x83 };
    emit(buf, pos, setType, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit32(buf, pos, TYPE_INT);
}

// ADDI R[A].intVal += imm
static void emitAddi(uint8_t* buf, size_t& pos, int A, int imm) {
    // add qword [rbx + A*16+8], imm32
    uint8_t code[] = { 0x48, 0x81, 0x83 };
    emit(buf, pos, code, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    emit32(buf, pos, static_cast<uint32_t>(imm));
}

// SUBI R[A].intVal -= imm
static void emitSubi(uint8_t* buf, size_t& pos, int A, int imm) {
    // sub qword [rbx + A*16+8], imm32
    uint8_t code[] = { 0x48, 0x81, 0xab };
    emit(buf, pos, code, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    emit32(buf, pos, static_cast<uint32_t>(imm));
}

// LOADINT R[A] = imm (set type=Int, intVal=imm)
static void emitLoadInt(uint8_t* buf, size_t& pos, int A, int imm) {
    // mov dword [rbx + A*16], TYPE_INT
    uint8_t setType[] = { 0xc7, 0x83 };
    emit(buf, pos, setType, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit32(buf, pos, TYPE_INT);
    // mov qword [rbx + A*16+8], imm  (using mov r/m64, imm32 sign-extended)
    uint8_t setVal[] = { 0x48, 0xc7, 0x83 };
    emit(buf, pos, setVal, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    emit32(buf, pos, static_cast<uint32_t>(imm));
}

// LOADK R[A] = constants[Bx] (copy 16 bytes)
static void emitLoadK(uint8_t* buf, size_t& pos, int A, BblValue* constPtr) {
    // movabs rsi, constPtr
    uint8_t movabs[] = { 0x48, 0xbe };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(constPtr));
    // movups xmm0, [rsi]
    uint8_t load[] = { 0x0f, 0x10, 0x06 };
    emit(buf, pos, load, 3);
    // movups [rbx + A*16], xmm0
    uint8_t store[] = { 0x0f, 0x11, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// LOADNULL R[A]: zero 16 bytes
static void emitLoadNull(uint8_t* buf, size_t& pos, int A) {
    // xorps xmm0, xmm0
    uint8_t xor1[] = { 0x0f, 0x57, 0xc0 };
    emit(buf, pos, xor1, 3);
    // movups [rbx + A*16], xmm0
    uint8_t store[] = { 0x0f, 0x11, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// MOVE R[A] = R[B] (copy 16 bytes)
static void emitMove(uint8_t* buf, size_t& pos, int A, int B) {
    // movups xmm0, [rbx + B*16]
    uint8_t load[] = { 0x0f, 0x10, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    // movups [rbx + A*16], xmm0
    uint8_t store[] = { 0x0f, 0x11, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// LTJMP: if R[A].intVal < R[B].intVal, jump to target (skip next stencil)
// Returns patch offset for the jl rel32
static size_t emitLtjmp(uint8_t* buf, size_t& pos, int A, int B) {
    // mov rax, [rbx + A*16+8]
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    // cmp rax, [rbx + B*16+8]
    uint8_t cmp1[] = { 0x48, 0x3b, 0x83 };
    emit(buf, pos, cmp1, 3);
    emit32(buf, pos, B * VAL_SIZE + DATA_OFF);
    // jl rel32 (skip next stencil when condition TRUE)
    uint8_t jl[] = { 0x0f, 0x8c };
    emit(buf, pos, jl, 2);
    size_t patchOff = pos;
    emit32(buf, pos, 0); // placeholder
    return patchOff;
}

// Same pattern for LE, GT, GE
static size_t emitLejmp(uint8_t* buf, size_t& pos, int A, int B) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3); emit32(buf, pos, A*VAL_SIZE+DATA_OFF);
    uint8_t cmp1[] = { 0x48, 0x3b, 0x83 };
    emit(buf, pos, cmp1, 3); emit32(buf, pos, B*VAL_SIZE+DATA_OFF);
    uint8_t jle[] = { 0x0f, 0x8e };
    emit(buf, pos, jle, 2);
    size_t p = pos; emit32(buf, pos, 0); return p;
}

static size_t emitGtjmp(uint8_t* buf, size_t& pos, int A, int B) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3); emit32(buf, pos, A*VAL_SIZE+DATA_OFF);
    uint8_t cmp1[] = { 0x48, 0x3b, 0x83 };
    emit(buf, pos, cmp1, 3); emit32(buf, pos, B*VAL_SIZE+DATA_OFF);
    uint8_t jg[] = { 0x0f, 0x8f };
    emit(buf, pos, jg, 2);
    size_t p = pos; emit32(buf, pos, 0); return p;
}

static size_t emitGejmp(uint8_t* buf, size_t& pos, int A, int B) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3); emit32(buf, pos, A*VAL_SIZE+DATA_OFF);
    uint8_t cmp1[] = { 0x48, 0x3b, 0x83 };
    emit(buf, pos, cmp1, 3); emit32(buf, pos, B*VAL_SIZE+DATA_OFF);
    uint8_t jge[] = { 0x0f, 0x8d };
    emit(buf, pos, jge, 2);
    size_t p = pos; emit32(buf, pos, 0); return p;
}

// JMP / LOOP: unconditional jump
static size_t emitJmp(uint8_t* buf, size_t& pos) {
    emit8(buf, pos, 0xe9); // jmp rel32
    size_t p = pos;
    emit32(buf, pos, 0);
    return p;
}

// Emit a call to a C function: movabs rax, <addr>; call rax
// Before calling, set up args: rdi=rbx(regs), rsi=r12(state)
// Additional args in edx, ecx, r8d per System V ABI
static void emitCallHelper2(uint8_t* buf, size_t& pos, void* fn, uint32_t arg3, uint32_t arg4) {
    // mov rdi, rbx
    uint8_t a1[] = { 0x48, 0x89, 0xdf };
    emit(buf, pos, a1, 3);
    // mov rsi, r12
    uint8_t a2[] = { 0x4c, 0x89, 0xe6 };
    emit(buf, pos, a2, 3);
    // mov edx, arg3
    uint8_t a3[] = { 0xba };
    emit(buf, pos, a3, 1);
    emit32(buf, pos, arg3);
    // mov ecx, arg4
    uint8_t a4[] = { 0xb9 };
    emit(buf, pos, a4, 1);
    emit32(buf, pos, arg4);
    // movabs rax, fn
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(fn));
    // call rax
    uint8_t call[] = { 0xff, 0xd0 };
    emit(buf, pos, call, 2);
}

// Call with 5 args (rdi, rsi, rdx, rcx, r8)
static void emitCallHelper3(uint8_t* buf, size_t& pos, void* fn, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    uint8_t a1[] = { 0x48, 0x89, 0xdf };
    emit(buf, pos, a1, 3);
    uint8_t a2[] = { 0x4c, 0x89, 0xe6 };
    emit(buf, pos, a2, 3);
    // mov rdx, r13 (chunk pointer)
    uint8_t a3[] = { 0x4c, 0x89, 0xea };
    emit(buf, pos, a3, 3);
    // mov ecx, arg4
    uint8_t a4[] = { 0xb9 };
    emit(buf, pos, a4, 1);
    emit32(buf, pos, arg4);
    // mov r8d, arg5
    uint8_t a5[] = { 0x41, 0xb8 };
    emit(buf, pos, a5, 2);
    emit32(buf, pos, arg5);
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(fn));
    uint8_t call[] = { 0xff, 0xd0 };
    emit(buf, pos, call, 2);
}

// isFalsy check: sets ZF if R[A] is falsy
// Result: jumps to target if falsy (for JMPFALSE) or truthy (for JMPTRUE)
static size_t emitJmpFalse(uint8_t* buf, size_t& pos, int A) {
    // cmp dword [rbx + A*16], 0  (Null?)
    uint8_t cmp0[] = { 0x83, 0xbb };
    emit(buf, pos, cmp0, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit8(buf, pos, TYPE_NULL);
    // je target
    uint8_t je[] = { 0x0f, 0x84 };
    emit(buf, pos, je, 2);
    size_t p1 = pos;
    emit32(buf, pos, 0);

    // cmp dword [rbx + A*16], 1  (Bool?)
    emit(buf, pos, cmp0, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit8(buf, pos, TYPE_BOOL);
    // jne not_falsy (it's truthy — not null, not bool)
    uint8_t jne[] = { 0x75 };
    emit(buf, pos, jne, 1);
    size_t jneOff = pos;
    emit8(buf, pos, 0);

    // It's bool — check if false
    // cmp byte [rbx + A*16+8], 0
    uint8_t cmpb[] = { 0x80, 0xbb };
    emit(buf, pos, cmpb, 2);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    emit8(buf, pos, 0);
    // je target (boolVal == false → falsy)
    emit(buf, pos, je, 2);
    size_t p2 = pos;
    emit32(buf, pos, 0);

    // Check Int == 0
    // Actually, isFalsy also checks Int==0. Let's simplify: just check type==Null || (type==Bool && !val) || (type==Int && val==0)
    // For now, patch jne to skip to here
    buf[jneOff] = static_cast<uint8_t>(pos - jneOff - 1);

    // cmp dword [rbx + A*16], 2  (Int?)
    emit(buf, pos, cmp0, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit8(buf, pos, TYPE_INT);
    // jne not_falsy
    emit(buf, pos, jne, 1);
    size_t jne2Off = pos;
    emit8(buf, pos, 0);

    // cmp qword [rbx + A*16+8], 0
    uint8_t cmpq[] = { 0x48, 0x83, 0xbb };
    emit(buf, pos, cmpq, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    emit8(buf, pos, 0);
    // je target (intVal == 0 → falsy)
    emit(buf, pos, je, 2);
    size_t p3 = pos;
    emit32(buf, pos, 0);

    // not_falsy:
    buf[jne2Off] = static_cast<uint8_t>(pos - jne2Off - 1);

    // All three je targets jump to the same place (the falsy target)
    // We'll return p1 and fix p2, p3 to also point there
    // Actually, we need one common target. Let's use a single jmp:
    // Better approach: emit jmp past, then falsy label jumps to target
    // Let me restructure with a single output jump:
    
    // At this point: not falsy, skip over the forward jump
    uint8_t jmpShort[] = { 0xeb };
    emit(buf, pos, jmpShort, 1);
    size_t skipOff = pos;
    emit8(buf, pos, 0);

    // falsy_label:
    size_t falsyLabel = pos;
    // jmp <target:rel32>
    emit8(buf, pos, 0xe9);
    size_t targetPatch = pos;
    emit32(buf, pos, 0);

    // Patch all je's to point to falsy_label
    patchRel32(buf, p1, falsyLabel);
    patchRel32(buf, p2, falsyLabel);
    patchRel32(buf, p3, falsyLabel);
    // Patch skip to jump past falsy_label
    buf[skipOff] = static_cast<uint8_t>(pos - skipOff - 1);

    return targetPatch;
}

// Comparisons: emit R[A] = R[B] <op> R[C] as bool
static void emitCmp(uint8_t* buf, size_t& pos, int A, int B, int C, uint8_t jccOpcode) {
    // mov rax, [rbx + B*16+8]
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE + DATA_OFF);
    // cmp rax, [rbx + C*16+8]
    uint8_t cmp1[] = { 0x48, 0x3b, 0x83 };
    emit(buf, pos, cmp1, 3);
    emit32(buf, pos, C * VAL_SIZE + DATA_OFF);
    // setCC al
    uint8_t setcc[] = { 0x0f, jccOpcode, 0xc0 };
    emit(buf, pos, setcc, 3);
    // movzx eax, al
    uint8_t movzx[] = { 0x0f, 0xb6, 0xc0 };
    emit(buf, pos, movzx, 3);
    // mov [rbx + A*16+8], rax (boolVal)
    uint8_t mov2[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, A * VAL_SIZE + DATA_OFF);
    // mov dword [rbx + A*16], TYPE_BOOL
    uint8_t setType[] = { 0xc7, 0x83 };
    emit(buf, pos, setType, 2);
    emit32(buf, pos, A * VAL_SIZE + TYPE_OFF);
    emit32(buf, pos, TYPE_BOOL);
}

struct JumpPatch {
    size_t patchOffset;  // native byte offset of rel32 to patch
    size_t targetInst;   // bytecode instruction index to jump to
};

JitCode jitCompile(BblState& state, Chunk& chunk) {
    JitCode jit;
    jit.capacity = chunk.code.size() * 64 + 512;
    jit.buf = static_cast<uint8_t*>(
        mmap(nullptr, jit.capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    jit.size = 0;

    emitPrologue(jit.buf, jit.size);

    std::vector<size_t> nativeOffsets(chunk.code.size() + 1, 0);
    std::vector<JumpPatch> patches;

    for (size_t i = 0; i < chunk.code.size(); i++) {
        nativeOffsets[i] = jit.size;
        uint32_t inst = chunk.code[i];
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst), B = decodeB(inst), C = decodeC(inst);
        uint16_t Bx = decodeBx(inst);
        int sBx = decodesBx(inst);

        switch (op) {
        case OP_LOADK:
            emitLoadK(jit.buf, jit.size, A, &chunk.constants[Bx]);
            break;
        case OP_LOADINT:
            emitLoadInt(jit.buf, jit.size, A, sBx);
            break;
        case OP_LOADNULL:
            emitLoadNull(jit.buf, jit.size, A);
            break;
        case OP_LOADBOOL:
            emitLoadInt(jit.buf, jit.size, A, 0); // reuse, then fix type
            // Fix type to Bool (1) and set boolVal
            {
                // mov dword [rbx + A*16], 1 (Bool)
                uint8_t st[] = { 0xc7, 0x83 };
                emit(jit.buf, jit.size, st, 2);
                emit32(jit.buf, jit.size, A * VAL_SIZE + TYPE_OFF);
                emit32(jit.buf, jit.size, 1); // Bool
                // mov byte [rbx + A*16+8], B
                uint8_t sb[] = { 0xc6, 0x83 };
                emit(jit.buf, jit.size, sb, 2);
                emit32(jit.buf, jit.size, A * VAL_SIZE + DATA_OFF);
                emit8(jit.buf, jit.size, B ? 1 : 0);
            }
            break;
        case OP_MOVE:
            emitMove(jit.buf, jit.size, A, B);
            break;
        case OP_ADD:
            emitAdd(jit.buf, jit.size, A, B, C);
            break;
        case OP_ADDI:
            emitAddi(jit.buf, jit.size, A, sBx);
            break;
        case OP_ADDK:
            // Load constant, add
            emitLoadK(jit.buf, jit.size, 0, &chunk.constants[C]); // temp into R0 area? No — use rax directly
            // Actually just do: mov rax, [rbx+B*16+8]; add rax, const; mov [rbx+A*16+8], rax
            jit.size -= 19; // undo emitLoadK
            {
                uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, mov1, 3);
                emit32(jit.buf, jit.size, B * VAL_SIZE + DATA_OFF);
                // movabs rcx, constVal
                int64_t cv = chunk.constants[C].intVal;
                uint8_t movabs[] = { 0x48, 0xb9 };
                emit(jit.buf, jit.size, movabs, 2);
                emit64(jit.buf, jit.size, static_cast<uint64_t>(cv));
                // add rax, rcx
                uint8_t addrr[] = { 0x48, 0x01, 0xc8 };
                emit(jit.buf, jit.size, addrr, 3);
                // store
                uint8_t mov2[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, mov2, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE + DATA_OFF);
                uint8_t setType[] = { 0xc7, 0x83 };
                emit(jit.buf, jit.size, setType, 2);
                emit32(jit.buf, jit.size, A * VAL_SIZE + TYPE_OFF);
                emit32(jit.buf, jit.size, TYPE_INT);
            }
            break;
        case OP_SUB:
            emitSub(jit.buf, jit.size, A, B, C);
            break;
        case OP_SUBI:
            emitSubi(jit.buf, jit.size, A, sBx);
            break;
        case OP_MUL:
            emitMul(jit.buf, jit.size, A, B, C);
            break;

        case OP_LTJMP: {
            size_t p = emitLtjmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2}); // skip next instruction (the JMP)
            break;
        }
        case OP_LEJMP: {
            size_t p = emitLejmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }
        case OP_GTJMP: {
            size_t p = emitGtjmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }
        case OP_GEJMP: {
            size_t p = emitGejmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }

        case OP_JMP: {
            size_t p = emitJmp(jit.buf, jit.size);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_LOOP: {
            // Backward jump — target is already compiled
            size_t p = emitJmp(jit.buf, jit.size);
            int target = static_cast<int>(i) + 1 - sBx;
            patchRel32(jit.buf, p, nativeOffsets[target]);
            break;
        }

        case OP_RETURN:
            emitReturn(jit.buf, jit.size, A);
            break;

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal);
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A);
            break;
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal);
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetGlobal, symId, A);
            break;
        }
        case OP_CALL:
            emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B);
            break;
        case OP_CLOSURE:
            emitCallHelper3(jit.buf, jit.size, (void*)jitClosure, 0, A, Bx);
            break;

        case OP_JMPFALSE: {
            size_t p = emitJmpFalse(jit.buf, jit.size, A);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_JMPTRUE: {
            // Invert: emit jmpfalse and swap targets
            // Actually: if truthy, jump. So: if NOT falsy, jump.
            // Simplest: emit the falsy check but invert the final jump
            // For now, use the general comparison approach
            // NOT falsy = truthy → jmp. We can negate by checking !isFalsy
            // Let's just emit comparison to null/bool/int and jump if NOT any of those
            // This is complex — fall through to default for now
            goto unsupported;
        }

        case OP_NOT:
            // Load isFalsy result into R[A]
            // Simplified: check type and set bool
            emitCmp(jit.buf, jit.size, A, B, B, 0x94); // sete (equal to self = always true, wrong)
            // Actually NOT is: R[A] = isFalsy(R[B])
            // Just call a helper for now
            goto unsupported;

        case OP_AND: {
            size_t p = emitJmpFalse(jit.buf, jit.size, A);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_OR: {
            // If truthy, skip (leave value). If falsy, continue.
            // This is the inverse of JMPFALSE — skip when NOT falsy
            // For simplicity, fall through
            goto unsupported;
        }

        case OP_EQ: emitCmp(jit.buf, jit.size, A, B, C, 0x94); break;  // sete
        case OP_NEQ: emitCmp(jit.buf, jit.size, A, B, C, 0x95); break; // setne
        case OP_LT: emitCmp(jit.buf, jit.size, A, B, C, 0x9c); break;  // setl
        case OP_GT: emitCmp(jit.buf, jit.size, A, B, C, 0x9f); break;  // setg
        case OP_LTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9e); break; // setle
        case OP_GTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9d); break; // setge

        case OP_DIV: {
            // mov rax, [rbx + B*16+8]; cqo; mov rcx, [rbx + C*16+8]; idiv rcx; store
            uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, mov1, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE + DATA_OFF);
            uint8_t cqo[] = { 0x48, 0x99 }; // cqo: sign-extend rax into rdx:rax
            emit(jit.buf, jit.size, cqo, 2);
            uint8_t movcx[] = { 0x48, 0x8b, 0x8b };
            emit(jit.buf, jit.size, movcx, 3);
            emit32(jit.buf, jit.size, C * VAL_SIZE + DATA_OFF);
            uint8_t idiv[] = { 0x48, 0xf7, 0xf9 }; // idiv rcx
            emit(jit.buf, jit.size, idiv, 3);
            uint8_t mov2[] = { 0x48, 0x89, 0x83 };
            emit(jit.buf, jit.size, mov2, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE + DATA_OFF);
            uint8_t setType[] = { 0xc7, 0x83 };
            emit(jit.buf, jit.size, setType, 2);
            emit32(jit.buf, jit.size, A * VAL_SIZE + TYPE_OFF);
            emit32(jit.buf, jit.size, TYPE_INT);
            break;
        }
        case OP_MOD: {
            uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, mov1, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE + DATA_OFF);
            uint8_t cqo[] = { 0x48, 0x99 };
            emit(jit.buf, jit.size, cqo, 2);
            uint8_t movcx[] = { 0x48, 0x8b, 0x8b };
            emit(jit.buf, jit.size, movcx, 3);
            emit32(jit.buf, jit.size, C * VAL_SIZE + DATA_OFF);
            uint8_t idiv[] = { 0x48, 0xf7, 0xf9 };
            emit(jit.buf, jit.size, idiv, 3);
            // remainder is in rdx
            uint8_t mov2[] = { 0x48, 0x89, 0x93 }; // mov [rbx+disp32], rdx
            emit(jit.buf, jit.size, mov2, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE + DATA_OFF);
            uint8_t setType[] = { 0xc7, 0x83 };
            emit(jit.buf, jit.size, setType, 2);
            emit32(jit.buf, jit.size, A * VAL_SIZE + TYPE_OFF);
            emit32(jit.buf, jit.size, TYPE_INT);
            break;
        }

        unsupported:
        default:
            // Unsupported opcode — fall back to interpreter
            // For now, just emit a return with null
            emitLoadNull(jit.buf, jit.size, A);
            emitReturn(jit.buf, jit.size, A);
            goto done_compile;
        }
    }

done_compile:
    nativeOffsets[chunk.code.size()] = jit.size;

    // Patch forward jumps
    for (auto& p : patches) {
        if (p.targetInst < nativeOffsets.size() && nativeOffsets[p.targetInst] != 0) {
            patchRel32(jit.buf, p.patchOffset, nativeOffsets[p.targetInst]);
        }
    }

    mprotect(jit.buf, jit.capacity, PROT_READ | PROT_EXEC);
    return jit;
}

BblValue jitExecute(BblState& state, Chunk& chunk) {
    JitCode jit = jitCompile(state, chunk);

    // Call: fn(regs, state, chunk)
    typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
    JitFn fn = reinterpret_cast<JitFn>(jit.buf);

    // Ensure register file is large enough
    if (state.vm->stack.size() < chunk.numRegs)
        state.vm->stack.resize(chunk.numRegs);

    BblValue result = fn(state.vm->stack.data(), &state, &chunk);

    jitFree(jit);
    return result;
}

void jitFree(JitCode& jit) {
    if (jit.buf) {
        munmap(jit.buf, jit.capacity);
        jit.buf = nullptr;
    }
}
