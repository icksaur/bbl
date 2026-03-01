#include "disasm.h"
#include "bbl.h"
#include <cinttypes>
#include <cstdio>

static int simpleInstruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

static int u8Instruction(const char* name, const Chunk& chunk, int offset) {
    uint8_t val = chunk.code[offset + 1];
    printf("%-20s %4d\n", name, val);
    return offset + 2;
}

static int u16Instruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t val = chunk.readU16(offset + 1);
    printf("%-20s %4d\n", name, val);
    return offset + 3;
}

static int constantInstruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t idx = chunk.readU16(offset + 1);
    printf("%-20s %4d", name, idx);
    if (idx < chunk.constants.size()) {
        auto& v = chunk.constants[idx];
        switch (v.type) {
            case BBL::Type::Int: printf(" (%" PRId64 ")", v.intVal); break;
            case BBL::Type::Float: printf(" (%g)", v.floatVal); break;
            case BBL::Type::String: printf(" (\"%s\")", v.stringVal->data.c_str()); break;
            case BBL::Type::Fn: printf(" (<fn>)"); break;
            default: break;
        }
    }
    printf("\n");
    return offset + 3;
}

static int jumpInstruction(const char* name, int sign, const Chunk& chunk, int offset) {
    uint16_t jump = chunk.readU16(offset + 1);
    int target = offset + 3 + sign * jump;
    printf("%-20s %4d -> %04d\n", name, jump, target);
    return offset + 3;
}

void disassembleChunk(const Chunk& chunk, const char* name) {
    printf("== %s ==\n", name);
    int offset = 0;
    while (offset < static_cast<int>(chunk.code.size())) {
        offset = disassembleInstruction(chunk, offset);
    }
}

int disassembleInstruction(const Chunk& chunk, int offset) {
    printf("%04d ", offset);
    if (offset > 0 && chunk.lines[offset] == chunk.lines[offset - 1])
        printf("   | ");
    else
        printf("%4d ", chunk.lines[offset]);

    uint8_t op = chunk.code[offset];
    switch (op) {
        case OP_CONSTANT:       return constantInstruction("OP_CONSTANT", chunk, offset);
        case OP_NULL:           return simpleInstruction("OP_NULL", offset);
        case OP_TRUE:           return simpleInstruction("OP_TRUE", offset);
        case OP_FALSE:          return simpleInstruction("OP_FALSE", offset);
        case OP_POP:            return simpleInstruction("OP_POP", offset);
        case OP_DUP:            return simpleInstruction("OP_DUP", offset);
        case OP_POPN:           return u8Instruction("OP_POPN", chunk, offset);
        case OP_ADD:            return simpleInstruction("OP_ADD", offset);
        case OP_SUB:            return simpleInstruction("OP_SUB", offset);
        case OP_MUL:            return simpleInstruction("OP_MUL", offset);
        case OP_DIV:            return simpleInstruction("OP_DIV", offset);
        case OP_MOD:            return simpleInstruction("OP_MOD", offset);
        case OP_BAND:           return simpleInstruction("OP_BAND", offset);
        case OP_BOR:            return simpleInstruction("OP_BOR", offset);
        case OP_BXOR:           return simpleInstruction("OP_BXOR", offset);
        case OP_BNOT:           return simpleInstruction("OP_BNOT", offset);
        case OP_SHL:            return simpleInstruction("OP_SHL", offset);
        case OP_SHR:            return simpleInstruction("OP_SHR", offset);
        case OP_EQ:             return simpleInstruction("OP_EQ", offset);
        case OP_NEQ:            return simpleInstruction("OP_NEQ", offset);
        case OP_LT:             return simpleInstruction("OP_LT", offset);
        case OP_GT:             return simpleInstruction("OP_GT", offset);
        case OP_LTE:            return simpleInstruction("OP_LTE", offset);
        case OP_GTE:            return simpleInstruction("OP_GTE", offset);
        case OP_NOT:            return simpleInstruction("OP_NOT", offset);
        case OP_GET_LOCAL:      return u16Instruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:      return u16Instruction("OP_SET_LOCAL", chunk, offset);
        case OP_GET_CAPTURE:    return u8Instruction("OP_GET_CAPTURE", chunk, offset);
        case OP_SET_CAPTURE:    return u8Instruction("OP_SET_CAPTURE", chunk, offset);
        case OP_GET_GLOBAL:     return u16Instruction("OP_GET_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:     return u16Instruction("OP_SET_GLOBAL", chunk, offset);
        case OP_JUMP:           return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:  return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_JUMP_IF_TRUE:   return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
        case OP_LOOP:           return jumpInstruction("OP_LOOP", -1, chunk, offset);
        case OP_AND:            return jumpInstruction("OP_AND", 1, chunk, offset);
        case OP_OR:             return jumpInstruction("OP_OR", 1, chunk, offset);
        case OP_CALL:           return u8Instruction("OP_CALL", chunk, offset);
        case OP_TAIL_CALL:      return u8Instruction("OP_TAIL_CALL", chunk, offset);
        case OP_RETURN:         return simpleInstruction("OP_RETURN", offset);
        case OP_CLOSURE: {
            uint16_t idx = chunk.readU16(offset + 1);
            uint8_t caps = chunk.code[offset + 3];
            printf("%-20s %4d caps=%d\n", "OP_CLOSURE", idx, caps);
            int o = offset + 4;
            for (int i = 0; i < caps; i++) {
                uint8_t srcType = chunk.code[o];
                uint16_t srcIdx = chunk.readU16(o + 1);
                printf("     |                    %s %d\n",
                       srcType == 0 ? "local" : "capture", srcIdx);
                o += 3;
            }
            return o;
        }
        case OP_VECTOR:         return u16Instruction("OP_VECTOR", chunk, offset);
        case OP_TABLE:          return u16Instruction("OP_TABLE", chunk, offset);
        case OP_STRUCT: {
            uint16_t idx = chunk.readU16(offset + 1);
            uint8_t argc = chunk.code[offset + 3];
            printf("%-20s %4d argc=%d\n", "OP_STRUCT", idx, argc);
            return offset + 4;
        }
        case OP_BINARY:         return simpleInstruction("OP_BINARY", offset);
        case OP_GET_FIELD:      return u16Instruction("OP_GET_FIELD", chunk, offset);
        case OP_SET_FIELD:      return u16Instruction("OP_SET_FIELD", chunk, offset);
        case OP_GET_INDEX:      return simpleInstruction("OP_GET_INDEX", offset);
        case OP_SET_INDEX:      return simpleInstruction("OP_SET_INDEX", offset);
        case OP_METHOD_CALL: {
            uint16_t idx = chunk.readU16(offset + 1);
            uint8_t argc = chunk.code[offset + 3];
            printf("%-20s %4d argc=%d\n", "OP_METHOD_CALL", idx, argc);
            return offset + 4;
        }
        case OP_TRY_BEGIN:      return jumpInstruction("OP_TRY_BEGIN", 1, chunk, offset);
        case OP_TRY_END:        return simpleInstruction("OP_TRY_END", offset);
        case OP_WITH_BEGIN:     return simpleInstruction("OP_WITH_BEGIN", offset);
        case OP_WITH_END:       return simpleInstruction("OP_WITH_END", offset);
        case OP_LENGTH:         return simpleInstruction("OP_LENGTH", offset);
        case OP_SIZEOF:         return u16Instruction("OP_SIZEOF", chunk, offset);
        case OP_EXEC:           return simpleInstruction("OP_EXEC", offset);
        case OP_EXECFILE:       return simpleInstruction("OP_EXECFILE", offset);
        default:
            printf("unknown opcode %d\n", op);
            return offset + 1;
    }
}
