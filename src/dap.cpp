#include "dap.h"
#include "bbl.h"
#include "chunk.h"
#include "vm.h"
#include <yyjson.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cinttypes>

static std::string dapValueToString(const BblValue& val) {
    char buf[64];
    switch (val.type()) {
        case BBL::Type::String: return "\"" + val.stringVal()->data + "\"";
        case BBL::Type::Int: snprintf(buf, sizeof(buf), "%" PRId64, val.intVal()); return buf;
        case BBL::Type::Float: snprintf(buf, sizeof(buf), "%g", val.floatVal()); return buf;
        case BBL::Type::Bool: return val.boolVal() ? "true" : "false";
        case BBL::Type::Null: return "null";
        case BBL::Type::Table: snprintf(buf, sizeof(buf), "table (%zu entries)", val.tableVal()->length()); return buf;
        case BBL::Type::Vector: snprintf(buf, sizeof(buf), "vector (%zu elements)", val.vectorVal()->length()); return buf;
        default: return "<object>";
    }
}

static const char* dapTypeName(const BblValue& val) {
    switch (val.type()) {
        case BBL::Type::String: return "string";
        case BBL::Type::Int: return "int";
        case BBL::Type::Float: return "float";
        case BBL::Type::Bool: return "bool";
        case BBL::Type::Null: return "null";
        case BBL::Type::Table: return "table";
        case BBL::Type::Vector: return "vector";
        case BBL::Type::Fn: return "function";
        default: return "object";
    }
}

static bool dapReadExact(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static std::string dapReadMessage(int fd) {
    std::string header;
    while (true) {
        char c;
        if (read(fd, &c, 1) != 1) return {};
        header += c;
        if (header.size() >= 4 && header.substr(header.size() - 4) == "\r\n\r\n") break;
        if (header.size() > 4096) return {};
    }
    size_t lenPos = header.find("Content-Length: ");
    if (lenPos == std::string::npos) return {};
    int len = std::atoi(header.c_str() + lenPos + 16);
    if (len <= 0 || len > 10 * 1024 * 1024) return {};
    std::string body(static_cast<size_t>(len), '\0');
    if (!dapReadExact(fd, body.data(), static_cast<size_t>(len))) return {};
    return body;
}

static bool dapSend(int fd, const std::string& json) {
    std::string msg = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t w = write(fd, msg.data() + sent, msg.size() - sent);
        if (w <= 0) return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

static std::string dapSerialize(yyjson_mut_doc* doc, yyjson_mut_val* root) {
    yyjson_mut_doc_set_root(doc, root);
    char* json = yyjson_mut_write(doc, 0, nullptr);
    std::string result(json);
    free(json);
    return result;
}

static void dapSendResponse(int fd, int& seq, int requestSeq, const char* command, bool success,
                            yyjson_mut_doc* doc, yyjson_mut_val* body = nullptr) {
    auto* root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, root, "seq", seq++);
    yyjson_mut_obj_add_str(doc, root, "type", "response");
    yyjson_mut_obj_add_int(doc, root, "request_seq", requestSeq);
    yyjson_mut_obj_add_bool(doc, root, "success", success);
    yyjson_mut_obj_add_str(doc, root, "command", command);
    if (body) yyjson_mut_obj_add_val(doc, root, "body", body);
    dapSend(fd, dapSerialize(doc, root));
}

static void dapSendEvent(int fd, int& seq, const char* event, yyjson_mut_doc* doc,
                         yyjson_mut_val* body = nullptr) {
    auto* root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, root, "seq", seq++);
    yyjson_mut_obj_add_str(doc, root, "type", "event");
    yyjson_mut_obj_add_str(doc, root, "event", event);
    if (body) yyjson_mut_obj_add_val(doc, root, "body", body);
    dapSend(fd, dapSerialize(doc, root));
}

static void handleDapClient(DapServer* dap) {
    BblState* state = dap->state;
    int fd = dap->clientFd;
    int& seq = dap->nextSeq;
    bool configDone = false;
    bool disconnected = false;

    auto waitForPause = [&]() {
        auto* dbg = state->debug;
        if (!dbg) return;
        std::unique_lock lock(dbg->mtx);
        dbg->cv.wait(lock, [&] {
            return dbg->paused.load(std::memory_order_acquire) ||
                   dbg->scriptDone.load(std::memory_order_acquire) ||
                   !dap->running.load();
        });
        if (dbg->scriptDone.load(std::memory_order_acquire)) {
            yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
            dapSendEvent(fd, seq, "terminated", doc);
            yyjson_mut_doc_free(doc);
            return;
        }
        if (!dbg->paused.load()) return;

        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        auto* body = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, body, "reason", "breakpoint");
        yyjson_mut_obj_add_int(doc, body, "threadId", 1);
        dapSendEvent(fd, seq, "stopped", doc, body);
        yyjson_mut_doc_free(doc);
    };

    while (dap->running.load() && !disconnected) {
        std::string msg = dapReadMessage(fd);
        if (msg.empty()) break;

        yyjson_doc* req = yyjson_read(msg.c_str(), msg.size(), 0);
        if (!req) continue;

        yyjson_val* root = yyjson_doc_get_root(req);
        int requestSeq = static_cast<int>(yyjson_get_int(yyjson_obj_get(root, "seq")));
        const char* command = yyjson_get_str(yyjson_obj_get(root, "command"));
        yyjson_val* args = yyjson_obj_get(root, "arguments");

        if (!command) { yyjson_doc_free(req); continue; }

        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);

        if (strcmp(command, "initialize") == 0) {
            auto* body = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_bool(doc, body, "supportsConfigurationDoneRequest", true);
            yyjson_mut_obj_add_bool(doc, body, "supportsEvaluateForHovers", true);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

            yyjson_mut_doc* evDoc = yyjson_mut_doc_new(nullptr);
            dapSendEvent(fd, seq, "initialized", evDoc);
            yyjson_mut_doc_free(evDoc);

        } else if (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0) {
            dapSendResponse(fd, seq, requestSeq, command, true, doc);

        } else if (strcmp(command, "setBreakpoints") == 0) {
            auto* dbg = state->debug;
            if (!dbg) { dbg = new DebugState(); state->debug = dbg; }
            state->debugEnabled.store(true, std::memory_order_release);

            const char* sourcePath = nullptr;
            yyjson_val* source = yyjson_obj_get(args, "source");
            if (source) sourcePath = yyjson_get_str(yyjson_obj_get(source, "path"));

            auto* body = yyjson_mut_obj(doc);
            auto* bpArr = yyjson_mut_arr(doc);

            if (sourcePath) {
                {
                    std::lock_guard lock(dbg->mtx);
                    dbg->breakpoints[sourcePath].clear();
                }
                yyjson_val* bps = yyjson_obj_get(args, "breakpoints");
                if (bps) {
                    yyjson_arr_iter it;
                    yyjson_arr_iter_init(bps, &it);
                    yyjson_val* bp;
                    while ((bp = yyjson_arr_iter_next(&it))) {
                        int line = static_cast<int>(yyjson_get_int(yyjson_obj_get(bp, "line")));
                        {
                            std::lock_guard lock(dbg->mtx);
                            dbg->breakpoints[sourcePath].insert(line);
                        }
                        auto* bpObj = yyjson_mut_obj(doc);
                        yyjson_mut_obj_add_bool(doc, bpObj, "verified", true);
                        yyjson_mut_obj_add_int(doc, bpObj, "line", line);
                        yyjson_mut_arr_append(bpArr, bpObj);
                    }
                }
            }
            yyjson_mut_obj_add_val(doc, body, "breakpoints", bpArr);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

        } else if (strcmp(command, "configurationDone") == 0) {
            configDone = true;
            dapSendResponse(fd, seq, requestSeq, command, true, doc);
            waitForPause();

        } else if (strcmp(command, "threads") == 0) {
            auto* body = yyjson_mut_obj(doc);
            auto* threads = yyjson_mut_arr(doc);
            auto* t = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_int(doc, t, "id", 1);
            yyjson_mut_obj_add_str(doc, t, "name", "main");
            yyjson_mut_arr_append(threads, t);
            yyjson_mut_obj_add_val(doc, body, "threads", threads);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

        } else if (strcmp(command, "stackTrace") == 0) {
            auto* dbg = state->debug;
            auto* body = yyjson_mut_obj(doc);
            auto* frames = yyjson_mut_arr(doc);

            if (dbg && dbg->paused.load()) {
                auto* topFrame = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_int(doc, topFrame, "id", 0);
                yyjson_mut_obj_add_str(doc, topFrame, "name", "<current>");
                auto* topSrc = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, topSrc, "path", dbg->pausedFile ? dbg->pausedFile : "");
                yyjson_mut_obj_add_val(doc, topFrame, "source", topSrc);
                yyjson_mut_obj_add_int(doc, topFrame, "line", dbg->pausedLine);
                yyjson_mut_obj_add_int(doc, topFrame, "column", 1);
                yyjson_mut_arr_append(frames, topFrame);

                for (int i = dbg->pausedTraceTop - 1; i >= 0; --i) {
                    auto& tf = state->traceStack[i];
                    auto* f = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_int(doc, f, "id", dbg->pausedTraceTop - i);
                    yyjson_mut_obj_add_str(doc, f, "name", tf.name ? tf.name : "<anonymous>");
                    auto* src = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_str(doc, src, "path", tf.file ? tf.file : "");
                    yyjson_mut_obj_add_val(doc, f, "source", src);
                    yyjson_mut_obj_add_int(doc, f, "line", tf.line);
                    yyjson_mut_obj_add_int(doc, f, "column", 1);
                    yyjson_mut_arr_append(frames, f);
                }
            }
            yyjson_mut_obj_add_val(doc, body, "stackFrames", frames);
            yyjson_mut_obj_add_int(doc, body, "totalFrames",
                state->debug ? state->debug->pausedTraceTop + 1 : 0);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

        } else if (strcmp(command, "scopes") == 0) {
            auto* body = yyjson_mut_obj(doc);
            auto* scopes = yyjson_mut_arr(doc);

            int frameId = args ? static_cast<int>(yyjson_get_int(yyjson_obj_get(args, "frameId"))) : 0;
            if (frameId == 0) {
                auto* scope = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, scope, "name", "Locals");
                yyjson_mut_obj_add_int(doc, scope, "variablesReference", 1);
                yyjson_mut_obj_add_bool(doc, scope, "expensive", false);
                yyjson_mut_arr_append(scopes, scope);
            }
            yyjson_mut_obj_add_val(doc, body, "scopes", scopes);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

        } else if (strcmp(command, "variables") == 0) {
            auto* dbg = state->debug;
            auto* body = yyjson_mut_obj(doc);
            auto* vars = yyjson_mut_arr(doc);
            int varRef = args ? static_cast<int>(yyjson_get_int(yyjson_obj_get(args, "variablesReference"))) : 0;

            if (varRef == 1 && dbg && dbg->paused.load()) {
                std::lock_guard lock(dbg->mtx);
                if (dbg->pausedChunk) {
                    for (auto& [reg, name] : dbg->pausedChunk->debugRegNames) {
                        if (reg >= dbg->pausedChunk->numRegs) continue;
                        BblValue val = dbg->pausedRegs[reg];
                        auto* v = yyjson_mut_obj(doc);
                        yyjson_mut_obj_add_str(doc, v, "name", name.c_str());
                        std::string valStr = dapValueToString(val);
                        yyjson_mut_obj_add_str(doc, v, "value", valStr.c_str());
                        yyjson_mut_obj_add_str(doc, v, "type", dapTypeName(val));

                        int childRef = 0;
                        if (val.type() == BBL::Type::Table || val.type() == BBL::Type::Vector) {
                            childRef = ++dap->nextVarRef;
                            dap->varRefs[childRef] = val.bits;
                        }
                        yyjson_mut_obj_add_int(doc, v, "variablesReference", childRef);
                        yyjson_mut_arr_append(vars, v);
                    }
                }
                for (auto& [symId, val] : state->vm->globals) {
                    if (val.isCFn()) continue;
                    const char* name = nullptr;
                    for (auto& [n, id] : state->symbolIds) {
                        if (id == symId) { name = n.c_str(); break; }
                    }
                    if (!name) continue;
                    auto* v = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_strcpy(doc, v, "name", name);
                    std::string valStr = dapValueToString(val);
                    yyjson_mut_obj_add_strcpy(doc, v, "value", valStr.c_str());
                    yyjson_mut_obj_add_str(doc, v, "type", dapTypeName(val));
                    int childRef = 0;
                    if (val.type() == BBL::Type::Table || val.type() == BBL::Type::Vector) {
                        childRef = ++dap->nextVarRef;
                        dap->varRefs[childRef] = val.bits;
                    }
                    yyjson_mut_obj_add_int(doc, v, "variablesReference", childRef);
                    yyjson_mut_arr_append(vars, v);
                }
            } else if (varRef > 1) {
                auto it = dap->varRefs.find(varRef);
                if (it != dap->varRefs.end()) {
                    BblValue container;
                    container.bits = it->second;
                    if (container.type() == BBL::Type::Table) {
                        auto* tbl = container.tableVal();
                        tbl->ensureOrder();
                        if (tbl->order) {
                            for (auto& key : *tbl->order) {
                                auto val = tbl->get(key);
                                if (!val) continue;
                                auto* v = yyjson_mut_obj(doc);
                                std::string keyStr = dapValueToString(key);
                                yyjson_mut_obj_add_strcpy(doc, v, "name", keyStr.c_str());
                                std::string valStr = dapValueToString(*val);
                                yyjson_mut_obj_add_strcpy(doc, v, "value", valStr.c_str());
                                yyjson_mut_obj_add_str(doc, v, "type", dapTypeName(*val));
                                int childRef = 0;
                                if (val->type() == BBL::Type::Table || val->type() == BBL::Type::Vector) {
                                    childRef = ++dap->nextVarRef;
                                    dap->varRefs[childRef] = val->bits;
                                }
                                yyjson_mut_obj_add_int(doc, v, "variablesReference", childRef);
                                yyjson_mut_arr_append(vars, v);
                            }
                        }
                    } else if (container.type() == BBL::Type::Vector) {
                        auto* vec = container.vectorVal();
                        for (size_t i = 0; i < vec->length(); ++i) {
                            BblValue elem = state->readVecElem(vec, i);
                            auto* v = yyjson_mut_obj(doc);
                            std::string idx = std::to_string(i);
                            yyjson_mut_obj_add_strcpy(doc, v, "name", idx.c_str());
                            std::string valStr = dapValueToString(elem);
                            yyjson_mut_obj_add_strcpy(doc, v, "value", valStr.c_str());
                            yyjson_mut_obj_add_str(doc, v, "type", dapTypeName(elem));
                            yyjson_mut_obj_add_int(doc, v, "variablesReference", 0);
                            yyjson_mut_arr_append(vars, v);
                        }
                    }
                }
            }
            yyjson_mut_obj_add_val(doc, body, "variables", vars);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);

        } else if (strcmp(command, "continue") == 0) {
            dap->varRefs.clear();
            dap->nextVarRef = 1;
            auto* dbg = state->debug;
            if (dbg) {
                dbg->stepMode.store(0);
                dbg->paused.store(false, std::memory_order_release);
                dbg->cv.notify_all();
            }
            auto* body = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_bool(doc, body, "allThreadsContinued", true);
            dapSendResponse(fd, seq, requestSeq, command, true, doc, body);
            waitForPause();

        } else if (strcmp(command, "next") == 0) {
            dap->varRefs.clear();
            dap->nextVarRef = 1;
            auto* dbg = state->debug;
            if (dbg) {
                dbg->stepDepth = dbg->pausedTraceTop;
                dbg->stepMode.store(2);
                dbg->paused.store(false, std::memory_order_release);
                dbg->cv.notify_all();
            }
            dapSendResponse(fd, seq, requestSeq, command, true, doc);
            waitForPause();

        } else if (strcmp(command, "stepIn") == 0) {
            dap->varRefs.clear();
            dap->nextVarRef = 1;
            auto* dbg = state->debug;
            if (dbg) {
                dbg->stepMode.store(1);
                dbg->paused.store(false, std::memory_order_release);
                dbg->cv.notify_all();
            }
            dapSendResponse(fd, seq, requestSeq, command, true, doc);
            waitForPause();

        } else if (strcmp(command, "stepOut") == 0) {
            dap->varRefs.clear();
            dap->nextVarRef = 1;
            auto* dbg = state->debug;
            if (dbg) {
                dbg->stepDepth = dbg->pausedTraceTop;
                dbg->stepMode.store(3);
                dbg->paused.store(false, std::memory_order_release);
                dbg->cv.notify_all();
            }
            dapSendResponse(fd, seq, requestSeq, command, true, doc);
            waitForPause();

        } else if (strcmp(command, "evaluate") == 0) {
            auto* dbg = state->debug;
            const char* expr = args ? yyjson_get_str(yyjson_obj_get(args, "expression")) : nullptr;
            auto* body = yyjson_mut_obj(doc);

            if (expr && dbg && dbg->paused.load()) {
                {
                    std::lock_guard lock(dbg->mtx);
                    dbg->evalRequest = expr;
                    dbg->hasEvalResult.store(false, std::memory_order_release);
                    dbg->hasEvalRequest.store(true, std::memory_order_release);
                }
                dbg->cv.notify_all();
                {
                    std::unique_lock lock(dbg->mtx);
                    dbg->cv.wait(lock, [&] { return dbg->hasEvalResult.load(std::memory_order_acquire); });
                }
                yyjson_mut_obj_add_strcpy(doc, body, "result", dbg->evalResult.c_str());
                yyjson_mut_obj_add_int(doc, body, "variablesReference", 0);
                dapSendResponse(fd, seq, requestSeq, command, !dbg->evalError, doc, body);
            } else {
                yyjson_mut_obj_add_str(doc, body, "result", "");
                yyjson_mut_obj_add_int(doc, body, "variablesReference", 0);
                dapSendResponse(fd, seq, requestSeq, command, false, doc, body);
            }

        } else if (strcmp(command, "disconnect") == 0) {
            disconnected = true;
            auto* dbg = state->debug;
            if (dbg && dbg->paused.load()) {
                dbg->stepMode.store(0);
                dbg->paused.store(false, std::memory_order_release);
                dbg->cv.notify_all();
            }
            dapSendResponse(fd, seq, requestSeq, command, true, doc);

        } else {
            dapSendResponse(fd, seq, requestSeq, command, true, doc);
        }

        yyjson_mut_doc_free(doc);
        yyjson_doc_free(req);
    }
}

void DapServer::start(int port) {
    if (running.load()) return;

    if (!state->debug) state->debug = new DebugState();
    state->debugEnabled.store(true, std::memory_order_release);

    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) return;

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(serverFd);
        serverFd = -1;
        return;
    }
    if (listen(serverFd, 1) < 0) {
        close(serverFd);
        serverFd = -1;
        return;
    }

    running.store(true);
    serverThread = std::thread([this] {
        while (running.load()) {
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &len);
            if (clientFd < 0) break;

            nextSeq = 1;
            varRefs.clear();
            nextVarRef = 1;
            handleDapClient(this);

            close(clientFd);
            clientFd = -1;
        }
    });
}

void DapServer::stop() {
    running.store(false);
    if (serverFd >= 0) {
        shutdown(serverFd, SHUT_RDWR);
        close(serverFd);
        serverFd = -1;
    }
    if (clientFd >= 0) {
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        clientFd = -1;
    }
    if (serverThread.joinable()) serverThread.join();
}
