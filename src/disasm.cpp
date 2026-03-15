#include "disasm.h"
#include "bbl.h"
#include <cinttypes>
#include <cstdio>

namespace bbl {


void disassembleChunk(const Chunk& chunk, const char* name) {
    printf("== %s (regs=%d) ==\n", name, chunk.numRegs);
    for (int i = 0; i < static_cast<int>(chunk.code.size()); i++)
        disassembleInstruction(chunk, i);
}

int disassembleInstruction(const Chunk& chunk, int offset) {
    uint32_t inst = chunk.code[offset];
    uint8_t op = decodeOP(inst);
    uint8_t A = decodeA(inst);
    uint8_t B = decodeB(inst);
    uint8_t C = decodeC(inst);
    uint16_t Bx = decodeBx(inst);
    int sBx = decodesBx(inst);

    printf("%04d ", offset);
    if (offset > 0 && chunk.lines[offset] == chunk.lines[offset - 1])
        printf("   | ");
    else
        printf("%4d ", chunk.lines[offset]);

    switch (op) {
    case OP_LOADK:     printf("LOADK     R%d K%d", A, Bx); if (Bx < chunk.constants.size()) { auto& v = chunk.constants[Bx]; if (v.type() == Type::Int) printf(" (%" PRId64 ")", v.intVal()); else if (v.type() == Type::String) printf(" (\"%s\")", v.stringVal()->data.c_str()); else if (v.type() == Type::Float) printf(" (%g)", v.floatVal()); } break;
    case OP_LOADNULL:  printf("LOADNULL  R%d", A); break;
    case OP_LOADBOOL:  printf("LOADBOOL  R%d %s", A, B ? "true" : "false"); break;
    case OP_LOADINT:   printf("LOADINT   R%d %d", A, sBx); break;
    case OP_ADD:       printf("ADD       R%d R%d R%d", A, B, C); break;
    case OP_ADDK:      printf("ADDK      R%d R%d K%d", A, B, C); break;
    case OP_ADDI:      printf("ADDI      R%d %d", A, sBx); break;
    case OP_SUBI:      printf("SUBI      R%d %d", A, sBx); break;
    case OP_SUB:       printf("SUB       R%d R%d R%d", A, B, C); break;
    case OP_MUL:       printf("MUL       R%d R%d R%d", A, B, C); break;
    case OP_DIV:       printf("DIV       R%d R%d R%d", A, B, C); break;
    case OP_MOD:       printf("MOD       R%d R%d R%d", A, B, C); break;
    case OP_BAND:      printf("BAND      R%d R%d R%d", A, B, C); break;
    case OP_BOR:       printf("BOR       R%d R%d R%d", A, B, C); break;
    case OP_BXOR:      printf("BXOR      R%d R%d R%d", A, B, C); break;
    case OP_BNOT:      printf("BNOT      R%d R%d", A, B); break;
    case OP_SHL:       printf("SHL       R%d R%d R%d", A, B, C); break;
    case OP_SHR:       printf("SHR       R%d R%d R%d", A, B, C); break;
    case OP_EQ:        printf("EQ        R%d R%d R%d", A, B, C); break;
    case OP_NEQ:       printf("NEQ       R%d R%d R%d", A, B, C); break;
    case OP_LT:        printf("LT        R%d R%d R%d", A, B, C); break;
    case OP_GT:        printf("GT        R%d R%d R%d", A, B, C); break;
    case OP_LTE:       printf("LTE       R%d R%d R%d", A, B, C); break;
    case OP_GTE:       printf("GTE       R%d R%d R%d", A, B, C); break;
    case OP_LTJMP:     printf("LTJMP     R%d R%d", A, B); break;
    case OP_LEJMP:     printf("LEJMP     R%d R%d", A, B); break;
    case OP_GTJMP:     printf("GTJMP     R%d R%d", A, B); break;
    case OP_GEJMP:     printf("GEJMP     R%d R%d", A, B); break;
    case OP_NOT:       printf("NOT       R%d R%d", A, B); break;
    case OP_MOVE:      printf("MOVE      R%d R%d", A, B); break;
    case OP_GETGLOBAL: printf("GETGLOBAL R%d K%d", A, Bx); break;
    case OP_SETGLOBAL: printf("SETGLOBAL R%d K%d", A, Bx); break;
    case OP_GETCAPTURE:printf("GETCAPTURE R%d %d", A, B); break;
    case OP_SETCAPTURE:printf("SETCAPTURE R%d %d", A, B); break;
    case OP_JMP:       printf("JMP       %d -> %04d", sBx, offset + 1 + sBx); break;
    case OP_JMPFALSE:  printf("JMPFALSE  R%d %d -> %04d", A, sBx, offset + 1 + sBx); break;
    case OP_JMPTRUE:   printf("JMPTRUE   R%d %d -> %04d", A, sBx, offset + 1 + sBx); break;
    case OP_LOOP:      printf("LOOP      %d -> %04d", sBx, offset + 1 - sBx); break;
    case OP_AND:       printf("AND       R%d %d -> %04d", A, sBx, offset + 1 + sBx); break;
    case OP_OR:        printf("OR        R%d %d -> %04d", A, sBx, offset + 1 + sBx); break;
    case OP_CALL:      printf("CALL      R%d %d %d", A, B, C); break;
    case OP_TAILCALL:  printf("TAILCALL  R%d %d", A, B); break;
    case OP_RETURN:    printf("RETURN    R%d", A); break;
    case OP_CLOSURE:   printf("CLOSURE   R%d K%d", A, Bx); break;
    case OP_VECTOR:    printf("VECTOR    R%d %d K%d", A, B, C); break;
    case OP_TABLE:     printf("TABLE     R%d %d", A, B); break;
    case OP_STRUCT:    printf("STRUCT    R%d %d K%d", A, B, C); break;
    case OP_BINARY:    printf("BINARY    R%d R%d", A, B); break;
    case OP_GETFIELD:  printf("GETFIELD  R%d R%d K%d", A, B, C); break;
    case OP_SETFIELD:  printf("SETFIELD  R%d R%d K%d", A, B, C); break;
    case OP_GETINDEX:  printf("GETINDEX  R%d R%d R%d", A, B, C); break;
    case OP_SETINDEX:  printf("SETINDEX  R%d R%d R%d", A, B, C); break;
    case OP_MCALL:     printf("MCALL     R%d %d K%d", A, B, C); break;
    case OP_TRYBEGIN:  printf("TRYBEGIN  R%d %d -> %04d", A, sBx, offset + 1 + sBx); break;
    case OP_TRYEND:    printf("TRYEND"); break;
    case OP_LENGTH:    printf("LENGTH    R%d R%d", A, B); break;
    case OP_SIZEOF:    printf("SIZEOF    R%d K%d", A, B); break;
    case OP_EXEC:      printf("EXEC      R%d R%d", A, B); break;
    case OP_EXECFILE:  printf("EXECFILE  R%d R%d", A, B); break;
    default:           printf("??? op=%d", op); break;
    }
    printf("\n");
    return offset + 1;
}

} // namespace bbl
