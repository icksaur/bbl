#pragma once

#include "chunk.h"
#include <cstdint>
#include <cstddef>

struct BblState;

struct JitCode {
    uint8_t* buf = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

JitCode jitCompile(BblState& state, Chunk& chunk);
BblValue jitExecute(BblState& state, Chunk& chunk);
void jitFree(JitCode& jit);
