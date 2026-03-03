#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct BblValue;

struct CaptureInfo {
    uint8_t srcType; // 0 = LOCAL, 1 = CAPTURE
    uint8_t srcIdx;
};

enum OpCode : uint8_t {
    OP_LOADK,
    OP_LOADNULL,
    OP_LOADBOOL,
    OP_LOADINT,

    OP_ADD,
    OP_ADDK,
    OP_ADDI,
    OP_SUB,
    OP_SUBI,
    OP_MUL,
    OP_DIV,
    OP_MOD,

    OP_BAND,
    OP_BOR,
    OP_BXOR,
    OP_BNOT,
    OP_SHL,
    OP_SHR,

    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LTE,
    OP_GTE,
    OP_LTJMP,
    OP_LEJMP,
    OP_GTJMP,
    OP_GEJMP,

    OP_NOT,

    OP_MOVE,
    OP_GETGLOBAL,
    OP_SETGLOBAL,
    OP_GETCAPTURE,
    OP_SETCAPTURE,

    OP_JMP,
    OP_JMPFALSE,
    OP_JMPTRUE,
    OP_LOOP,

    OP_AND,
    OP_OR,

    OP_CLOSURE,
    OP_CALL,
    OP_TAILCALL,
    OP_RETURN,

    OP_VECTOR,
    OP_TABLE,
    OP_STRUCT,
    OP_BINARY,

    OP_GETFIELD,
    OP_SETFIELD,
    OP_GETINDEX,
    OP_SETINDEX,
    OP_MCALL,

    OP_TRYBEGIN,
    OP_TRYEND,

    OP_LENGTH,
    OP_SIZEOF,
    OP_EXEC,
    OP_EXECFILE,
};

// 32-bit instruction encoding
// iABC:  op(8) | A(8) | B(8) | C(8)
// iABx:  op(8) | A(8) | Bx(16)
// iAsBx: op(8) | A(8) | sBx(16)  (sBx stored as unsigned + bias)

constexpr int SBXBIAS = 32767;

inline uint32_t encodeABC(uint8_t op, uint8_t A, uint8_t B, uint8_t C) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(A) << 8) |
           (static_cast<uint32_t>(B) << 16) | (static_cast<uint32_t>(C) << 24);
}

inline uint32_t encodeABx(uint8_t op, uint8_t A, uint16_t Bx) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(A) << 8) |
           (static_cast<uint32_t>(Bx) << 16);
}

inline uint32_t encodeAsBx(uint8_t op, uint8_t A, int sBx) {
    uint16_t usBx = static_cast<uint16_t>(sBx + SBXBIAS);
    return encodeABx(op, A, usBx);
}

inline uint8_t decodeOP(uint32_t inst)  { return inst & 0xFF; }
inline uint8_t decodeA(uint32_t inst)   { return (inst >> 8) & 0xFF; }
inline uint8_t decodeB(uint32_t inst)   { return (inst >> 16) & 0xFF; }
inline uint8_t decodeC(uint32_t inst)   { return (inst >> 24) & 0xFF; }
inline uint16_t decodeBx(uint32_t inst) { return (inst >> 16) & 0xFFFF; }
inline int decodesBx(uint32_t inst)     { return static_cast<int>(decodeBx(inst)) - SBXBIAS; }

struct Chunk {
    std::vector<uint32_t> code;
    std::vector<BblValue> constants;
    std::vector<int> lines;
    uint8_t numRegs = 2;
    uint16_t hotCount = 0;
    bool traceCompiled = false;
    void* traceCode = nullptr;
    size_t traceCapacity = 0;

    void emitABC(uint8_t op, uint8_t A, uint8_t B, uint8_t C, int line) {
        code.push_back(encodeABC(op, A, B, C));
        lines.push_back(line);
    }

    void emitABx(uint8_t op, uint8_t A, uint16_t Bx, int line) {
        code.push_back(encodeABx(op, A, Bx));
        lines.push_back(line);
    }

    void emitAsBx(uint8_t op, uint8_t A, int sBx, int line) {
        code.push_back(encodeAsBx(op, A, sBx));
        lines.push_back(line);
    }

    size_t addConstant(const BblValue& val);

    void patchsBx(size_t offset, int sBx) {
        uint8_t op = decodeOP(code[offset]);
        uint8_t A = decodeA(code[offset]);
        code[offset] = encodeAsBx(op, A, sBx);
    }

    void patchBx(size_t offset, uint16_t Bx) {
        uint8_t op = decodeOP(code[offset]);
        uint8_t A = decodeA(code[offset]);
        code[offset] = encodeABx(op, A, Bx);
    }
};
