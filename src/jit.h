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
void jitFree(JitCode& jit);

struct TraceEntry {
    uint32_t inst;
    Chunk* chunk;
    uint8_t regBase;
    bool branchTaken = false;
};

struct Snapshot {
    Chunk* chunk;
    size_t pc;
    uint8_t regBase;
};

struct Trace {
    std::vector<TraceEntry> entries;
    std::vector<Snapshot> snapshots;
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
JitCode compileTrace(BblState& state, Trace& trace);
TraceResult executeTrace(JitCode& jit, BblValue* regs, BblState* state);
