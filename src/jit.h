#pragma once

#include "chunk.h"
#include <cstdint>
#include <cstddef>

struct BblState;
struct BblClosure;

struct JitCode {
    uint8_t* buf = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

JitCode jitCompile(BblState& state, Chunk& chunk, BblClosure* self = nullptr);
BblValue jitExecute(BblState& state, Chunk& chunk);
BblValue jitCallChecked(BblValue* regs, BblState* state, uint8_t argc);
void jitFree(JitCode& jit);

extern "C" {
    void jitCall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc);
}

struct TraceEntry {
    uint32_t inst;
    Chunk* chunk;
    uint8_t regBase;
    bool branchTaken = false;
    bool eliminated = false;
    uint8_t resultType = 0;
};

struct Snapshot {
    Chunk* chunk;
    size_t pc;
    uint8_t regBase;
};

struct SunkField {
    std::string name;
    uint8_t srcReg;
};

struct SunkAllocation {
    uint8_t destReg;
    std::vector<SunkField> fields;
};

struct Trace {
    std::vector<TraceEntry> entries;
    std::vector<Snapshot> snapshots;
    std::vector<SunkAllocation> sunkAllocs;
    Chunk* startChunk = nullptr;
    size_t startPc = 0;
    int maxRegs = 0;
    bool valid = false;
};

struct TraceResult {
    bool completed;     // true = trace ran to completion (looped back)
    size_t exitPc;      // if !completed, resume interpreting here
};

Trace recordTrace(BblState& state, Chunk& chunk, size_t loopPc, BblValue* regs);
void optimizeTrace(BblState& state, Trace& trace);
JitCode compileTrace(BblState& state, Trace& trace);
TraceResult executeTrace(JitCode& jit, BblValue* regs, BblState* state);
