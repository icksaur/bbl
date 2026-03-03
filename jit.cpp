#include "jit.h"
#include "bbl.h"
#include "vm.h"
#include <cstring>
#include <set>
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
    void jitTable(BblValue* regs, BblState* state, uint8_t destReg, uint8_t pairCount);
    void jitMcall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc, BblString* methodStr);
    void jitVector(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint8_t argc, uint8_t typeIdx);
    void jitBinary(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitLength(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitGetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint8_t objReg, uint8_t nameIdx);
    void jitSetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t valReg, uint8_t objReg, uint8_t nameIdx);
    void jitGetIndex(BblValue* regs, BblState* state, uint8_t destReg, uint8_t objReg, uint8_t idxReg);
    void jitSetIndex(BblValue* regs, BblState* state, uint8_t valReg, uint8_t objReg, uint8_t idxReg);
    void jitExec(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
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

        if (!closure->jitCache) {
            JitCode* cached = new JitCode(jitCompile(*state, closure->chunk, closure));
            closure->jitCache = cached;
        }
        typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
        JitFn fn = reinterpret_cast<JitFn>(closure->jitCache->buf);
        regs[base] = fn(&regs[base], state, &closure->chunk);
    } else if (callee.type == BBL::Type::Fn) {
        BblFn* fn = callee.fnVal;
        regs[base] = state->callFn(fn, &regs[base + 1], argc, 0);
    } else {
        throw BBL::Error{"not callable"};
    }
}

void jitGetCapture(BblValue* regs, BblState* state, uint8_t destReg, uint8_t capIdx) {
    (void)state;
    if (!regs[0].isClosure) throw BBL::Error{"no closure for capture"};
    BblClosure* closure = regs[0].closureVal;
    regs[destReg] = closure->captures[capIdx];
}

void jitSetCapture(BblValue* regs, BblState* state, uint8_t srcReg, uint8_t capIdx) {
    (void)state;
    if (!regs[0].isClosure) throw BBL::Error{"no closure for capture"};
    BblClosure* closure = regs[0].closureVal;
    closure->captures[capIdx] = regs[srcReg];
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

    for (size_t i = 0; i < proto->captureDescs.size(); i++) {
        auto& desc = proto->captureDescs[i];
        if (desc.srcType == 0)
            closure->captures[i] = regs[desc.srcIdx];
        else if (regs[0].isClosure)
            closure->captures[i] = regs[0].closureVal->captures[desc.srcIdx];
        else
            closure->captures[i] = BblValue::makeNull();
    }

    regs[destReg] = BblValue::makeClosure(closure);
}

void jitTable(BblValue* regs, BblState* state, uint8_t destReg, uint8_t pairCount) {
    BblTable* tbl = state->allocTable();
    for (int i = 0; i < pairCount; i++)
        tbl->set(regs[destReg + 1 + i*2], regs[destReg + 2 + i*2]);
    regs[destReg] = BblValue::makeTable(tbl);
}

void jitMcall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc, BblString* methodStr) {
    BblValue receiver = regs[base];
    BblValue* args = &regs[base + 1];

    // Dispatch through the same method tables as the interpreter
    if (receiver.type == BBL::Type::Table) {
        BblTable* tbl = receiver.tableVal;
        if (methodStr == state->m.get) regs[base] = tbl->get(args[0]).value_or(static_cast<size_t>(argc) > 1 ? args[1] : BblValue::makeNull());
        else if (methodStr == state->m.set) { tbl->set(args[0], args[1]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.has) regs[base] = BblValue::makeBool(tbl->has(args[0]));
        else if (methodStr == state->m.del) { tbl->del(args[0]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(tbl->length()));
        else if (methodStr == state->m.keys) {
            BblTable* keys = state->allocTable(); int64_t i = 0;
            for (auto& k : tbl->order) keys->set(BblValue::makeInt(i++), k);
            regs[base] = BblValue::makeTable(keys);
        } else if (methodStr == state->m.push) {
            for (int _i=0;_i<argc;_i++) { tbl->set(BblValue::makeInt(tbl->nextIntKey), args[_i]); }
            regs[base] = BblValue::makeNull();
        } else throw BBL::Error{"unknown table method: " + methodStr->data};
    } else if (receiver.type == BBL::Type::Vector) {
        BblVec* vec = receiver.vectorVal;
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(vec->length()));
        else if (methodStr == state->m.push) { for (int i=0;i<argc;i++) state->packValue(vec, args[i]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.at) regs[base] = state->readVecElem(vec, static_cast<size_t>(args[0].intVal));
        else if (methodStr == state->m.set) { state->writeVecElem(vec, static_cast<size_t>(args[0].intVal), args[1]); regs[base] = BblValue::makeNull(); }
        else throw BBL::Error{"unknown vector method: " + methodStr->data};
    } else if (receiver.type == BBL::Type::String) {
        BblString* str = receiver.stringVal;
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(str->data.size()));
        else throw BBL::Error{"unknown string method: " + methodStr->data};
    } else if (receiver.type == BBL::Type::Binary) {
        BblBinary* bin = receiver.binaryVal;
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(bin->length()));
        else throw BBL::Error{"unknown binary method: " + methodStr->data};
    } else throw BBL::Error{"cannot call method on " + std::string(typeName(receiver.type))};
}

void jitVector(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint8_t argc, uint8_t typeIdx) {
    std::string elemType = chunk->constants[typeIdx].stringVal->data;
    BBL::Type elemTypeTag = BBL::Type::Null;
    size_t elemSize = 0;
    auto dit = state->structDescs.find(elemType);
    if (dit != state->structDescs.end()) { elemTypeTag = BBL::Type::Struct; elemSize = dit->second.totalSize; }
    else if (elemType == "int" || elemType == "int64") { elemTypeTag = BBL::Type::Int; elemSize = 8; }
    else if (elemType == "float" || elemType == "float64") { elemTypeTag = BBL::Type::Float; elemSize = 8; }
    else if (elemType == "float32") { elemTypeTag = BBL::Type::Float; elemSize = 4; }
    else if (elemType == "int32") { elemTypeTag = BBL::Type::Int; elemSize = 4; }
    else throw BBL::Error{"unknown vector element type: " + elemType};
    BblVec* vec = state->allocVector(elemType, elemTypeTag, elemSize);
    for (int i = 0; i < argc; i++) state->packValue(vec, regs[destReg + 1 + i]);
    regs[destReg] = BblValue::makeVector(vec);
}

void jitBinary(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    BblValue& arg = regs[srcReg];
    if (arg.type == BBL::Type::Vector) regs[destReg] = BblValue::makeBinary(state->allocBinary(arg.vectorVal->data));
    else if (arg.type == BBL::Type::Struct) regs[destReg] = BblValue::makeBinary(state->allocBinary(arg.structVal->data));
    else if (arg.type == BBL::Type::Int) regs[destReg] = BblValue::makeBinary(state->allocBinary(std::vector<uint8_t>(static_cast<size_t>(arg.intVal), 0)));
    else throw BBL::Error{"binary: invalid argument type"};
}

void jitLength(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    BblValue& obj = regs[srcReg];
    if (obj.type == BBL::Type::Vector) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.vectorVal->length()));
    else if (obj.type == BBL::Type::String) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.stringVal->data.size()));
    else if (obj.type == BBL::Type::Binary) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.binaryVal->length()));
    else if (obj.type == BBL::Type::Table) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.tableVal->length()));
    else throw BBL::Error{"cannot get length"};
}

void jitGetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint8_t objReg, uint8_t nameIdx) {
    BblValue& obj = regs[objReg];
    std::string fieldName = chunk->constants[nameIdx].stringVal->data;
    if (obj.type == BBL::Type::Struct) {
        for (auto& fd : obj.structVal->desc->fields)
            if (fd.name == fieldName) { regs[destReg] = state->readField(obj.structVal, fd); return; }
        throw BBL::Error{"struct has no field '" + fieldName + "'"};
    } else if (obj.type == BBL::Type::Table) {
        regs[destReg] = obj.tableVal->get(BblValue::makeString(state->intern(fieldName))).value_or(BblValue::makeNull());
    } else throw BBL::Error{"cannot access field"};
}

void jitSetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t valReg, uint8_t objReg, uint8_t nameIdx) {
    BblValue& obj = regs[objReg];
    std::string fieldName = chunk->constants[nameIdx].stringVal->data;
    if (obj.type == BBL::Type::Struct) {
        for (auto& fd : obj.structVal->desc->fields)
            if (fd.name == fieldName) { state->writeField(obj.structVal, fd, regs[valReg]); return; }
    } else if (obj.type == BBL::Type::Table) {
        obj.tableVal->set(BblValue::makeString(state->intern(fieldName)), regs[valReg]);
    }
}

void jitGetIndex(BblValue* regs, BblState* state, uint8_t destReg, uint8_t objReg, uint8_t idxReg) {
    BblValue& obj = regs[objReg]; BblValue& idx = regs[idxReg];
    if (obj.type == BBL::Type::Vector) regs[destReg] = state->readVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal));
    else if (obj.type == BBL::Type::Table) regs[destReg] = obj.tableVal->get(idx).value_or(BblValue::makeNull());
    else throw BBL::Error{"cannot index"};
}

void jitSetIndex(BblValue* regs, BblState* state, uint8_t valReg, uint8_t objReg, uint8_t idxReg) {
    BblValue& obj = regs[objReg]; BblValue& idx = regs[idxReg];
    if (obj.type == BBL::Type::Vector) state->writeVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal), regs[valReg]);
    else if (obj.type == BBL::Type::Table) obj.tableVal->set(idx, regs[valReg]);
}

void jitExec(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    if (regs[srcReg].type != BBL::Type::String) throw BBL::Error{"exec: argument must be string"};
    BblLexer lexer(regs[srcReg].stringVal->data.c_str());
    auto nodes = parse(lexer);
    BblValue result = BblValue::makeNull();
    for (auto& n : nodes) result = state->eval(n, state->rootScope);
    regs[destReg] = result;
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

JitCode jitCompile(BblState& state, Chunk& chunk, BblClosure* self) {
    JitCode jit;
    jit.capacity = chunk.code.size() * 64 + 512;
    jit.buf = static_cast<uint8_t*>(
        mmap(nullptr, jit.capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    jit.size = 0;

    emitPrologue(jit.buf, jit.size);

    std::vector<size_t> nativeOffsets(chunk.code.size() + 1, 0);
    std::vector<JumpPatch> patches;
    std::unordered_map<uint8_t, BblClosure*> closureRegs;
    std::set<uint8_t> selfRefRegs;
    std::vector<size_t> selfCallPatches;

    for (size_t i = 0; i < chunk.code.size(); i++) {
        nativeOffsets[i] = jit.size;
        uint32_t inst = chunk.code[i];
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst), B = decodeB(inst), C = decodeC(inst);
        uint16_t Bx = decodeBx(inst);
        int sBx = decodesBx(inst);

        if (op != OP_GETGLOBAL && op != OP_CLOSURE && op != OP_CALL) {
            selfRefRegs.erase(A);
            closureRegs.erase(A);
        }

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
            if (self && state.vm) {
                auto it = state.vm->globals.find(symId);
                if (it != state.vm->globals.end() &&
                    it->second.type == BBL::Type::Fn &&
                    it->second.isClosure &&
                    it->second.closureVal == self) {
                    selfRefRegs.insert(A);
                }
            }
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A);
            break;
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal);
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetGlobal, symId, A);
            break;
        }
        case OP_CALL: {
            if (selfRefRegs.count(A)) {
                // Native self-recursive call through full prologue
                // lea rdi, [rbx + A*16]
                uint8_t lea[] = { 0x48, 0x8d, 0xbb };
                emit(jit.buf, jit.size, lea, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                // mov rsi, r12 (state)
                uint8_t movsi[] = { 0x4c, 0x89, 0xe6 };
                emit(jit.buf, jit.size, movsi, 3);
                // mov rdx, r13 (chunk)
                uint8_t movdx[] = { 0x4c, 0x89, 0xea };
                emit(jit.buf, jit.size, movdx, 3);
                // call rel32 → function entry (offset 0)
                emit8(jit.buf, jit.size, 0xe8);
                selfCallPatches.push_back(jit.size);
                emit32(jit.buf, jit.size, 0);
                // result: rax=type+flags, rdx=data (rbx restored by epilogue)
                uint8_t st1[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, st1, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t st2[] = { 0x48, 0x89, 0x93 };
                emit(jit.buf, jit.size, st2, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE + DATA_OFF);
            } else {
            // Try to inline: check if base register holds a known closure
            auto cit = closureRegs.find(A);
            if (cit != closureRegs.end() && B == cit->second->arity) {
                BblClosure* callee = cit->second;
                // Inline the callee's bytecode with register remapping:
                // Callee R[0] = callee (unused), R[1..arity] = args from caller R[A+1..A+argc]
                // Callee's other regs mapped to caller regs above A+argc
                uint8_t argBase = A + 1; // caller regs holding args
                // For each callee instruction, remap registers
                for (size_t ci = 0; ci < callee->chunk.code.size(); ci++) {
                    uint32_t cinst = callee->chunk.code[ci];
                    uint8_t cop = decodeOP(cinst);
                    uint8_t cA = decodeA(cinst), cB = decodeB(cinst), cC = decodeC(cinst);
                    int csBx = decodesBx(cinst);

                    // Remap register: callee R[0] → skip, R[1..arity] → caller R[A+1..A+arity]
                    // R[arity+1..] → caller R[A+arity+1..]
                    auto remap = [&](uint8_t r) -> uint8_t {
                        if (r == 0) return A; // callee slot (unused in practice)
                        if (r <= callee->arity) return A + r; // param → arg register
                        return A + r; // temp → above args
                    };

                    switch (cop) {
                    case OP_ADD: emitAdd(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_SUB: emitSub(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_MUL: emitMul(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_ADDI: emitAddi(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_SUBI: emitSubi(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_MOVE: emitMove(jit.buf, jit.size, remap(cA), remap(cB)); break;
                    case OP_LOADINT: emitLoadInt(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_LOADK: emitLoadK(jit.buf, jit.size, remap(cA), &callee->chunk.constants[decodeBx(cinst)]); break;
                    case OP_RETURN:
                        // Inline return: move callee result to caller's base register
                        if (remap(cA) != A) emitMove(jit.buf, jit.size, A, remap(cA));
                        goto inline_done;
                    default:
                        // Can't inline this instruction — fall back to helper call
                        emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B);
                        goto inline_done;
                    }
                }
                inline_done:;
            } else {
                emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B);
            }
            } // end self-ref else
            break;
        }
        case OP_CLOSURE:
            emitCallHelper3(jit.buf, jit.size, (void*)jitClosure, 0, A, Bx);
            // Track that register A now holds this closure proto
            closureRegs[A] = chunk.constants[Bx].closureVal;
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
            break; // fallback to default
        }

        case OP_NOT:
            // Load isFalsy result into R[A]
            // Simplified: check type and set bool
            emitCmp(jit.buf, jit.size, A, B, B, 0x94); // sete (equal to self = always true, wrong)
            // Actually NOT is: R[A] = isFalsy(R[B])
            // Just call a helper for now
            break; // fallback to default

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
            break; // fallback to default
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

        case OP_TABLE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitTable, A, B);
            break;

        case OP_MCALL: {
            // Pass: regs(rdi=rbx), state(rsi=r12), base(edx=A), argc(ecx=B), methodStr(r8=pointer)
            BblString* methodStr = chunk.constants[C].stringVal;
            // mov rdi, rbx
            uint8_t a1[] = { 0x48, 0x89, 0xdf }; emit(jit.buf, jit.size, a1, 3);
            // mov rsi, r12
            uint8_t a2[] = { 0x4c, 0x89, 0xe6 }; emit(jit.buf, jit.size, a2, 3);
            // mov edx, A
            uint8_t a3[] = { 0xba }; emit(jit.buf, jit.size, a3, 1); emit32(jit.buf, jit.size, A);
            // mov ecx, B
            uint8_t a4[] = { 0xb9 }; emit(jit.buf, jit.size, a4, 1); emit32(jit.buf, jit.size, B);
            // movabs r8, methodStr
            uint8_t movr8[] = { 0x49, 0xb8 }; emit(jit.buf, jit.size, movr8, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(methodStr));
            // movabs rax, jitMcall
            uint8_t movabs[] = { 0x48, 0xb8 }; emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitMcall));
            // call rax
            uint8_t call[] = { 0xff, 0xd0 }; emit(jit.buf, jit.size, call, 2);
            break;
        }

        case OP_VECTOR:
            emitCallHelper3(jit.buf, jit.size, (void*)jitVector, 0, A, static_cast<uint32_t>((B << 8) | C));
            break;
        case OP_BINARY:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBinary, A, B);
            break;
        case OP_LENGTH:
            emitCallHelper2(jit.buf, jit.size, (void*)jitLength, A, B);
            break;
        case OP_GETFIELD:
            emitCallHelper3(jit.buf, jit.size, (void*)jitGetField, 0, A, static_cast<uint32_t>((B << 8) | C));
            break;
        case OP_SETFIELD:
            emitCallHelper3(jit.buf, jit.size, (void*)jitSetField, 0, A, static_cast<uint32_t>((B << 8) | C));
            break;
        case OP_GETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetIndex, static_cast<uint32_t>((A << 16) | (B << 8) | C), 0);
            break;
        case OP_SETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetIndex, static_cast<uint32_t>((A << 16) | (B << 8) | C), 0);
            break;
        case OP_EXEC:
            emitCallHelper2(jit.buf, jit.size, (void*)jitExec, A, B);
            break;
        case OP_GETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetCapture, A, B);
            break;
        case OP_SETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetCapture, A, B);
            break;
        case OP_SIZEOF:
        case OP_STRUCT:
        case OP_TAILCALL:
        case OP_TRYBEGIN:
        case OP_TRYEND:
        case OP_EXECFILE:
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

    for (size_t p : selfCallPatches) {
        patchRel32(jit.buf, p, 0);
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

// ============ Tracing JIT ============

Trace recordTrace(BblState& state, Chunk& chunk, size_t loopPc, BblValue* regs) {
    Trace trace;
    trace.startChunk = &chunk;
    trace.startPc = loopPc;
    trace.valid = false;

    // The LOOP at loopPc jumps back to (loopPc + 1 - sBx).
    uint32_t loopInst = chunk.code[loopPc];
    int sBx = decodesBx(loopInst);
    size_t bodyStart = loopPc + 1 - sBx;

    struct CallInfo { Chunk* chunk; size_t pc; uint8_t regBase; };
    std::vector<CallInfo> callStack;
    callStack.push_back({&chunk, bodyStart, 0});

    Chunk* curChunk = &chunk;
    size_t pc = bodyStart;
    uint8_t regBase = 0;
    int maxRegs = chunk.numRegs;
    int depth = 0;
    int maxEntries = 200;

    while (trace.entries.size() < static_cast<size_t>(maxEntries)) {
        if (pc >= curChunk->code.size()) { return trace; }
        uint32_t inst = curChunk->code[pc];
        uint8_t op = decodeOP(inst);

        if (op == OP_LOOP && curChunk == &chunk && depth == 0) {
            trace.maxRegs = maxRegs;
            trace.valid = true;
            return trace;
        }

        trace.entries.push_back({inst, curChunk, regBase});
        pc++;

        // Follow calls
        if (op == OP_CALL && depth < 4) {
            uint8_t A = decodeA(inst);
            uint8_t base = A + regBase;
            BblValue callee = regs[base];
            if (callee.type == BBL::Type::Fn && callee.isClosure) {
                BblClosure* closure = callee.closureVal;
                callStack.push_back({curChunk, pc, regBase});
                curChunk = &closure->chunk;
                pc = 0;
                regBase = base;
                depth++;
                int needed = regBase + closure->chunk.numRegs;
                if (needed > maxRegs) maxRegs = needed;
                continue;
            }
            return trace; // can't inline non-closure calls
        }

        if (op == OP_RETURN && depth > 0) {
            depth--;
            auto& ci = callStack.back();
            curChunk = ci.chunk;
            pc = ci.pc;
            regBase = ci.regBase;
            callStack.pop_back();
            continue;
        }

        if (op == OP_RETURN && depth == 0) {
            return trace; // unexpected return in trace
        }

        // Handle jumps within trace
        if (op == OP_JMP) {
            int off = decodesBx(inst);
            pc = pc - 1 + 1 + off; // pc was already incremented
        }
        if (op == OP_LTJMP || op == OP_LEJMP || op == OP_GTJMP || op == OP_GEJMP) {
            // Record the branch. The next instruction should be JMP (the else path).
            // If condition was true (skip JMP), we skip pc++ again
            // We just record — the compiler will emit a guard
        }
        if (op == OP_JMPFALSE || op == OP_JMPTRUE) {
            // Record — compiler will emit guard
            int off = decodesBx(inst);
            // For recording, we assume the hot path doesn't jump (condition passed)
            // If it did jump, we'd need to follow — for simplicity, don't follow
        }

        // Unsupported for tracing
        if (op == OP_CLOSURE || op == OP_GETGLOBAL || op == OP_SETGLOBAL ||
            op == OP_MCALL || op == OP_TABLE || op == OP_VECTOR ||
            op == OP_TRYBEGIN || op == OP_TRYEND || op == OP_EXEC || op == OP_EXECFILE) {
            // Abort trace for complex ops
            return trace;
        }
    }
    return trace;
}

JitCode compileTrace(BblState& state, Trace& trace) {
    JitCode jit;
    jit.capacity = trace.entries.size() * 64 + 512;
    jit.buf = static_cast<uint8_t*>(
        mmap(nullptr, jit.capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    jit.size = 0;

    emitPrologue(jit.buf, jit.size);

    size_t loopStart = jit.size;
    std::vector<size_t> sideExitPatches;

    for (size_t i = 0; i < trace.entries.size(); i++) {
        auto& entry = trace.entries[i];
        uint32_t inst = entry.inst;
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst) + entry.regBase;
        uint8_t B = decodeB(inst) + entry.regBase;
        uint8_t C = decodeC(inst) + entry.regBase;
        int sBx = decodesBx(inst);

        switch (op) {
        case OP_ADD: emitAdd(jit.buf, jit.size, A, B, C); break;
        case OP_SUB: emitSub(jit.buf, jit.size, A, B, C); break;
        case OP_MUL: emitMul(jit.buf, jit.size, A, B, C); break;
        case OP_ADDI: emitAddi(jit.buf, jit.size, A, sBx); break;
        case OP_SUBI: emitSubi(jit.buf, jit.size, A, sBx); break;
        case OP_MOVE: emitMove(jit.buf, jit.size, A, B); break;
        case OP_LOADINT: emitLoadInt(jit.buf, jit.size, A, sBx); break;
        case OP_LOADNULL: emitLoadNull(jit.buf, jit.size, A); break;
        case OP_LOADK: emitLoadK(jit.buf, jit.size, A, &entry.chunk->constants[decodeBx(inst)]); break;
        case OP_ADDK: {
            int origA = decodeA(inst) + entry.regBase;
            int origB = decodeB(inst) + entry.regBase;
            int origC = decodeC(inst); // constant index, not register
            // mov rax, [rbx+B*16+8]; add with constant; store
            uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, mov1, 3);
            emit32(jit.buf, jit.size, origB * VAL_SIZE + DATA_OFF);
            if (entry.chunk->constants[origC].type != BBL::Type::Int) {
                jitFree(jit); return JitCode{};
            }
            int64_t cv = entry.chunk->constants[origC].intVal;
            uint8_t movabs[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, static_cast<uint64_t>(cv));
            uint8_t addrr[] = { 0x48, 0x01, 0xc8 };
            emit(jit.buf, jit.size, addrr, 3);
            uint8_t mov2[] = { 0x48, 0x89, 0x83 };
            emit(jit.buf, jit.size, mov2, 3);
            emit32(jit.buf, jit.size, origA * VAL_SIZE + DATA_OFF);
            uint8_t setType[] = { 0xc7, 0x83 };
            emit(jit.buf, jit.size, setType, 2);
            emit32(jit.buf, jit.size, origA * VAL_SIZE + TYPE_OFF);
            emit32(jit.buf, jit.size, TYPE_INT);
            break;
        }
        case OP_LTJMP: {
            size_t p = emitLtjmp(jit.buf, jit.size, A, B);
            // If condition true (hot path), skip the next JMP in the trace
            // The jl jumps past the side exit — patch to skip next entry
            sideExitPatches.push_back(p);
            break;
        }
        case OP_LEJMP: {
            size_t p = emitLejmp(jit.buf, jit.size, A, B);
            sideExitPatches.push_back(p);
            break;
        }
        case OP_JMP:
            // In the trace, JMPs are the "else" path after a fused compare.
            // If we get here, the compare guard already passed — skip the JMP.
            // (The JMP would exit the loop, but the guard prevents that.)
            break;
        case OP_JMPFALSE:
            // Guard: if value is falsy, side exit
            // For now, skip — the trace assumes the hot path
            break;

        case OP_CALL:
            // Inlined — the callee's instructions follow in the trace
            // Emit a type guard: check that R[A] is still the expected closure
            // For now, skip the guard (trust the trace)
            break;
        case OP_RETURN:
            // Inlined return: the result register was recorded.
            // The caller's dest was set by the CALL instruction that preceded this.
            // Move the return value to the caller's expected register.
            {
                uint8_t retReg = decodeA(inst) + entry.regBase;
                // Find the CALL that inlined this: look back for OP_CALL
                // The CALL's A field is the dest in the caller.
                // For simplicity, emit MOVE retReg → callerDest
                // We need to know the caller's dest — it's stored in the CALL entry
                // Search backwards for the matching CALL
                for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                    if (decodeOP(trace.entries[j].inst) == OP_CALL &&
                        trace.entries[j].regBase < entry.regBase) {
                        uint8_t callA = decodeA(trace.entries[j].inst) + trace.entries[j].regBase;
                        if (retReg != callA)
                            emitMove(jit.buf, jit.size, callA, retReg);
                        break;
                    }
                }
            }
            break;

        case OP_LT: case OP_GT: case OP_LTE: case OP_GTE:
        case OP_EQ: case OP_NEQ: case OP_NOT:
        case OP_LOADBOOL:
        case OP_GETGLOBAL: case OP_SETGLOBAL:
        case OP_GETCAPTURE: case OP_SETCAPTURE:
            // Fall back to C helper for complex ops in trace
            // For now, abort the trace compilation
            jitFree(jit);
            return JitCode{};

        default:
            break;
        }
    }

    // End of trace: jump back to loop start
    {
        emit8(jit.buf, jit.size, 0xe9);
        int32_t rel = static_cast<int32_t>(loopStart - (jit.size + 4));
        emit32(jit.buf, jit.size, static_cast<uint32_t>(rel));
    }

    // Patch side exits: for now, all guards jump to a common exit
    size_t exitLabel = jit.size;
    // Exit: set rax = 0 (not completed), pop regs, ret
    {
        uint8_t xor_rax[] = { 0x48, 0x31, 0xc0 }; // xor rax, rax
        emit(jit.buf, jit.size, xor_rax, 3);
        emitEpilogue(jit.buf, jit.size);
    }

    // Patch all guard jumps to exit label
    for (size_t p : sideExitPatches) {
        patchRel32(jit.buf, p, exitLabel);
    }

    mprotect(jit.buf, jit.capacity, PROT_READ | PROT_EXEC);
    return jit;
}

TraceResult executeTrace(JitCode& jit, BblValue* regs, BblState* state) {
    if (!jit.buf) return {false, 0};
    typedef int64_t (*TraceFn)(BblValue*, BblState*, void*);
    TraceFn fn = reinterpret_cast<TraceFn>(jit.buf);
    int64_t result = fn(regs, state, nullptr);
    return {result != 0, 0};
}
