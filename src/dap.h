#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

namespace bbl {

struct BblState;
struct BblValue;

struct DapServer {
    BblState* state = nullptr;
    int serverFd = -1;
    int clientFd = -1;
    std::thread serverThread;
    std::atomic<bool> running{false};
    int nextSeq = 1;

    int nextVarRef = 1;
    std::unordered_map<int, uint64_t> varRefs;

    void start(int port);
    void stop();
};


} // namespace bbl