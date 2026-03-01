#pragma once

#include "bbl.h"
#include "chunk.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct BblClosure {
    Chunk chunk;
    int arity = 0;
    std::string name;
    std::vector<BblValue> captures;
    bool marked = false;
};

struct CallFrame {
    Chunk* chunk = nullptr;
    uint8_t* ip = nullptr;
    BblValue* slots = nullptr;
    BblClosure* closure = nullptr;
    int line = 0;
};

constexpr int VM_MAX_FRAMES = 256;
constexpr int VM_MAX_STACK  = 8192;

struct VmState {
    CallFrame frames[VM_MAX_FRAMES];
    int frameCount = 0;
    std::vector<BblValue> stack;
    BblValue* stackTop = nullptr;
    std::unordered_map<uint32_t, BblValue> globals;

    struct ExHandler {
        int frameIdx;
        uint8_t* catchIp;
        uint16_t localSlot;
        BblValue* stackBase;
    };
    std::vector<ExHandler> exHandlers;

    VmState() : stack(VM_MAX_STACK) { stackTop = stack.data(); }

    void reset() {
        frameCount = 0;
        stackTop = stack.data();
        exHandlers.clear();
    }
};

enum InterpretResult {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
};

InterpretResult vmExecute(BblState& state, Chunk& chunk);
