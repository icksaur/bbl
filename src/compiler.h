#pragma once

#include "chunk.h"
#include "bbl.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbl {

struct BblState;
struct AstNode;
struct BblClosure;

struct CompilerState {
    Chunk chunk;
    std::unordered_map<uint32_t, uint8_t> localRegs;
    std::unordered_map<uint32_t, const AstNode*> inlinableFns;
    std::vector<CaptureInfo> captures;
    int scopeDepth = 0;
    int arity = 0;
    std::string fnName;
    CompilerState* enclosing = nullptr;
    uint8_t nextReg = 0;
    uint8_t maxRegs = 0;
    int compileDepth = 0;

    struct LoopInfo {
        int start;
        bool isEach = false;
        std::vector<int> breaks;
        std::vector<int> continues;
    };
    std::vector<LoopInfo> loops;

    uint8_t allocReg() {
        if (nextReg >= 255) throw Error{"too many registers (limit 255)"};
        uint8_t r = nextReg++;
        if (nextReg > maxRegs) maxRegs = nextReg;
        return r;
    }
    void freeReg() { if (nextReg > 0) nextReg--; }
    void freeRegsTo(uint8_t r) { nextReg = r; }

    int resolveLocal(uint32_t symbolId) const {
        auto it = localRegs.find(symbolId);
        return it != localRegs.end() ? it->second : -1;
    }

    int resolveCapture(uint32_t symbolId);
};

Chunk compile(BblState& state, const std::vector<AstNode>& nodes);


} // namespace bbl