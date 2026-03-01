#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct BblValue;

enum OpCode : uint8_t {
    OP_CONSTANT,
    OP_NULL,
    OP_TRUE,
    OP_FALSE,

    OP_POP,
    OP_DUP,
    OP_POPN,

    OP_ADD,
    OP_SUB,
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

    OP_NOT,

    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_CAPTURE,
    OP_SET_CAPTURE,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,

    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_JUMP_IF_TRUE,
    OP_LOOP,

    OP_AND,
    OP_OR,

    OP_CLOSURE,
    OP_CALL,
    OP_TAIL_CALL,
    OP_RETURN,

    OP_VECTOR,
    OP_TABLE,
    OP_STRUCT,
    OP_BINARY,

    OP_GET_FIELD,
    OP_SET_FIELD,
    OP_GET_INDEX,
    OP_SET_INDEX,
    OP_METHOD_CALL,

    OP_TRY_BEGIN,
    OP_TRY_END,
    OP_WITH_BEGIN,
    OP_WITH_END,

    OP_LENGTH,
    OP_SIZEOF,
    OP_EXEC,
    OP_EXECFILE,
};

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<BblValue> constants;
    std::vector<int> lines;

    void emit(uint8_t byte, int line) {
        code.push_back(byte);
        lines.push_back(line);
    }

    void emitU16(uint16_t val, int line) {
        emit(static_cast<uint8_t>((val >> 8) & 0xff), line);
        emit(static_cast<uint8_t>(val & 0xff), line);
    }

    size_t addConstant(const BblValue& val);

    void patchU16(size_t offset, uint16_t val) {
        code[offset]     = static_cast<uint8_t>((val >> 8) & 0xff);
        code[offset + 1] = static_cast<uint8_t>(val & 0xff);
    }

    uint16_t readU16(size_t offset) const {
        return static_cast<uint16_t>((code[offset] << 8) | code[offset + 1]);
    }
};
