#pragma once

#include "chunk.h"
#include <cstdint>
#include <string>
#include <vector>

struct BblState;
struct AstNode;
struct BblClosure;

struct Local {
    uint32_t symbolId;
    int depth;
};

struct CaptureInfo {
    uint8_t srcType; // 0 = LOCAL, 1 = CAPTURE
    uint16_t srcIdx;
};

struct CompilerState {
    Chunk chunk;
    std::vector<Local> locals;
    std::vector<CaptureInfo> captures;
    int scopeDepth = 0;
    int arity = 0;
    std::string fnName;
    CompilerState* enclosing = nullptr;

    struct LoopInfo {
        int start;
        int scopeDepthAtLoop;
        std::vector<int> breaks;
    };
    std::vector<LoopInfo> loops;

    int resolveLocal(uint32_t symbolId) const;
    int resolveCapture(uint32_t symbolId);
};

Chunk compile(BblState& state, const std::vector<AstNode>& nodes);
