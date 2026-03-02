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
