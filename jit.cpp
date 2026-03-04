#include "jit.h"
#include "bbl.h"
#include "vm.h"
#include <cmath>
#include <cstring>
#include <set>
#include <vector>
#include <sys/mman.h>

// BblValue layout: NaN-boxed uint64_t, sizeof=8
// Register R[i] at byte offset i*8 from rbx

static constexpr int VAL_SIZE = 8;
static constexpr uint64_t NB_TAG_INT  = 0xFFFA000000000000ULL;
static constexpr uint64_t NB_TAG_BOOL = 0xFFF9000000000000ULL;
static constexpr uint64_t NB_TAG_NULL = 0xFFF8000000000000ULL;
static constexpr uint64_t NB_PAYLOAD  = 0x0000FFFFFFFFFFFFULL;

// C helper functions callable from JIT'd code
extern "C" {
    void jitGetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg);
    void jitSetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg);
    void jitCall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc);
    void jitGetCapture(BblValue* regs, BblState* state, uint8_t destReg, uint8_t capIdx);
    void jitSetCapture(BblValue* regs, BblState* state, uint8_t srcReg, uint8_t capIdx);
    void jitClosure(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint16_t protoIdx);
    void jitTable(BblValue* regs, BblState* state, uint8_t destReg, uint8_t pairCount);
    void jitMcall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc, BblString* methodStr);
    void jitVector(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint32_t packed);
    void jitBinary(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitLength(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitGetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint32_t packed);
    void jitSetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t valReg, uint32_t packed);
    void jitGetIndex(BblValue* regs, BblState* state, uint32_t packed, uint32_t unused);
    void jitSetIndex(BblValue* regs, BblState* state, uint32_t packed, uint32_t unused);
    void jitExec(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitArith(BblValue* regs, BblState* state, uint8_t A, uint32_t packed);
    void jitNot(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitStruct(BblValue* regs, BblState* state, Chunk* chunk, uint8_t A, uint32_t packed);
    void jitSizeof(BblValue* regs, BblState* state, Chunk* chunk, uint8_t A, uint8_t constIdx);
    void jitExecFile(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitBitwise(BblValue* regs, BblState* state, uint8_t A, uint32_t packed);
    void jitStoreError(BblValue* regs, BblState* state, uint8_t destReg, uint8_t unused);
}

static thread_local bool g_jitError = false;
static thread_local std::string g_jitErrorMsg;
static std::string jitValToStr(BblState& state, const BblValue& v);

#define JIT_ERROR(state, msg) do { \
    g_jitError = true; \
    g_jitErrorMsg = (msg); \
    return; \
} while(0)

#define JIT_TRY try {
#define JIT_CATCH } catch (const BBL::Error& _e) { \
    g_jitError = true; \
    g_jitErrorMsg = _e.what; \
    return; \
} catch (const BblTerminated&) { \
    g_jitError = true; \
    g_jitErrorMsg = "terminated"; \
    return; \
} catch (const std::exception& _e) { \
    g_jitError = true; \
    g_jitErrorMsg = _e.what(); \
    return; \
}


void jitGetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg) {
    auto it = state->vm->globals.find(symId);
    if (it != state->vm->globals.end()) { regs[destReg] = it->second; return; }
    auto* val = state->rootScope.lookup(symId);
    if (val) { regs[destReg] = *val; return; }
    JIT_ERROR(state, "undefined variable");
}

void jitSetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg) {
    state->vm->globals[symId] = regs[srcReg];
    state->rootScope.def(symId, regs[srcReg]);
}

void jitCall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc) {
    try {
    BblValue callee = regs[base];
    if (callee.type() == BBL::Type::Fn && callee.isCFn()) {
        state->callArgs.clear();
        for (int i = 0; i < argc; i++)
            state->callArgs.push_back(regs[base + 1 + i]);
        state->hasReturn = false;
        state->returnValue = BblValue::makeNull();
        callee.cfnVal()(state);
        regs[base] = state->hasReturn ? state->returnValue : BblValue::makeNull();
    } else if (callee.type() == BBL::Type::Fn && callee.isClosure()) {
        BblClosure* closure = callee.closureVal();

        if (!closure->jitCache) {
            JitCode* cached = new JitCode(jitCompile(*state, closure->chunk, closure));
            closure->jitCache = cached;
        }
        typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
        JitFn fn = reinterpret_cast<JitFn>(closure->jitCache->buf);
        regs[base] = fn(&regs[base], state, &closure->chunk);
        if (g_jitError) return;
    } else if (callee.type() == BBL::Type::Fn) {
        BblFn* fn = callee.fnVal();
        regs[base] = state->callFn(fn, &regs[base + 1], argc, 0);
    } else {
        JIT_ERROR(state, "not callable");
    }
    } catch (const BBL::Error& e) {
        JIT_ERROR(state, e.what);
    } catch (const BblTerminated&) {
        JIT_ERROR(state, "terminated");
    } catch (const std::exception& e) {
        JIT_ERROR(state, e.what());
    }
}

void jitGetCapture(BblValue* regs, BblState* state, uint8_t destReg, uint8_t capIdx) {
    (void)state;
    if (!regs[0].isClosure()) JIT_ERROR(state, "no closure for capture");
    BblClosure* closure = regs[0].closureVal();
    regs[destReg] = closure->captures[capIdx];
}

void jitSetCapture(BblValue* regs, BblState* state, uint8_t srcReg, uint8_t capIdx) {
    (void)state;
    if (!regs[0].isClosure()) JIT_ERROR(state, "no closure for capture");
    BblClosure* closure = regs[0].closureVal();
    closure->captures[capIdx] = regs[srcReg];
}

void jitClosure(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint16_t protoIdx) {
    BblClosure* proto = chunk->constants[protoIdx].closureVal();
    BblClosure* closure = new BblClosure();
    closure->chunk = proto->chunk;
    closure->arity = proto->arity;
    closure->name = proto->name;
    closure->captureDescs = proto->captureDescs;
    closure->captures.resize(proto->captureDescs.size());
    state->allocatedClosures.push_back(closure);

    for (size_t i = 0; i < proto->captureDescs.size(); i++) {
        auto& desc = proto->captureDescs[i];
        if (desc.srcType == 0)
            closure->captures[i] = regs[desc.srcIdx];
        else if (regs[0].isClosure())
            closure->captures[i] = regs[0].closureVal()->captures[desc.srcIdx];
        else
            closure->captures[i] = BblValue::makeNull();
    }

    regs[destReg] = BblValue::makeClosure(closure);
}

void jitTable(BblValue* regs, BblState* state, uint8_t destReg, uint8_t pairCount) {
    BblTable* tbl = state->allocTable();
    for (int i = 0; i < pairCount; i++)
        tbl->set(regs[destReg + 1 + i*2], regs[destReg + 2 + i*2]);
    regs[destReg] = BblValue::makeTable(tbl);
}

void jitMcall(BblValue* regs, BblState* state, uint8_t base, uint8_t argc, BblString* methodStr) {
    try {
    BblValue receiver = regs[base];
    BblValue* args = &regs[base + 1];

    // Dispatch through the same method tables as the interpreter
    if (receiver.type() == BBL::Type::Table) {
        BblTable* tbl = receiver.tableVal();
        if (methodStr == state->m.get) regs[base] = tbl->get(args[0]).value_or(static_cast<size_t>(argc) > 1 ? args[1] : BblValue::makeNull());
        else if (methodStr == state->m.set) { tbl->set(args[0], args[1]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.has) regs[base] = BblValue::makeBool(tbl->has(args[0]));
        else if (methodStr == state->m.del) { tbl->del(args[0]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(tbl->length()));
        else if (methodStr == state->m.keys) {
            BblTable* keys = state->allocTable(); int64_t i = 0;
            for (auto& k : tbl->order) keys->set(BblValue::makeInt(i++), k);
            regs[base] = BblValue::makeTable(keys);
        } else if (methodStr == state->m.push) {
            for (int _i=0;_i<argc;_i++) { tbl->set(BblValue::makeInt(tbl->nextIntKey), args[_i]); }
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.pop) {
            bool found = false;
            for (auto it = tbl->order.rbegin(); it != tbl->order.rend(); ++it) {
                if (it->type() == BBL::Type::Int) {
                    regs[base] = tbl->get(*it).value_or(BblValue::makeNull());
                    tbl->del(*it);
                    found = true;
                    break;
                }
            }
            if (!found) JIT_ERROR(state, "pop: no integer keys");
        } else if (methodStr == state->m.at) {
            size_t idx = static_cast<size_t>(args[0].intVal());
            if (idx >= tbl->order.size()) JIT_ERROR(state, "table index out of range");
            BblValue key = tbl->order[idx];
            regs[base] = tbl->get(key).value_or(BblValue::makeNull());
        } else {
            auto val = tbl->get(BblValue::makeString(const_cast<BblString*>(methodStr)));
            if (val.has_value() && val->type() == BBL::Type::Fn) {
                regs[base + 1] = BblValue::makeTable(tbl);
                for (int i = 0; i < argc; i++) regs[base + 2 + i] = args[i];
                regs[base] = val.value();
                jitCall(regs, state, base, argc + 1);
            } else {
                JIT_ERROR(state, "unknown table method: " + methodStr->data);
            }
        }
    } else if (receiver.type() == BBL::Type::Vector) {
        BblVec* vec = receiver.vectorVal();
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(vec->length()));
        else if (methodStr == state->m.push) { for (int i=0;i<argc;i++) state->packValue(vec, args[i]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.pop) {
            if (vec->length() == 0) JIT_ERROR(state, "pop on empty vector");
            regs[base] = state->readVecElem(vec, vec->length() - 1);
            vec->data.resize(vec->data.size() - vec->elemSize);
        } else if (methodStr == state->m.clear) { vec->data.clear(); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.at) regs[base] = state->readVecElem(vec, static_cast<size_t>(args[0].intVal()));
        else if (methodStr == state->m.set) { state->writeVecElem(vec, static_cast<size_t>(args[0].intVal()), args[1]); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.resize) { vec->data.resize(static_cast<size_t>(args[0].intVal()) * vec->elemSize, 0); regs[base] = BblValue::makeNull(); }
        else if (methodStr == state->m.reserve) {
            int64_t cap = args[0].intVal();
            if (cap < 0) JIT_ERROR(state, "vector.reserve: capacity must be non-negative");
            vec->data.reserve(static_cast<size_t>(cap) * vec->elemSize); regs[base] = BblValue::makeNull();
        }
        else JIT_ERROR(state, "unknown vector method: " + methodStr->data);
    } else if (receiver.type() == BBL::Type::String) {
        BblString* str = receiver.stringVal();
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(str->data.size()));
        else if (methodStr == state->m.at) {
            size_t idx = static_cast<size_t>(args[0].intVal());
            if (idx >= str->data.size()) JIT_ERROR(state, "string index " + std::to_string(idx) + " out of bounds (length " + std::to_string(str->data.size()) + ")");
            regs[base] = BblValue::makeString(state->intern(std::string(1, str->data[idx])));
        }
        else if (methodStr == state->m.slice) {
            int64_t start = args[0].intVal();
            int64_t len = argc > 1 ? args[1].intVal() : static_cast<int64_t>(str->data.size()) - start;
            regs[base] = BblValue::makeString(state->intern(str->data.substr(static_cast<size_t>(start), static_cast<size_t>(len))));
        } else if (methodStr == state->m.find) {
            size_t start = argc > 1 ? static_cast<size_t>(args[1].intVal()) : 0;
            auto pos = str->data.find(args[0].stringVal()->data, start);
            regs[base] = BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
        } else if (methodStr == state->m.contains) regs[base] = BblValue::makeBool(str->data.find(args[0].stringVal()->data) != std::string::npos);
        else if (methodStr == state->m.starts_with) regs[base] = BblValue::makeBool(str->data.starts_with(args[0].stringVal()->data));
        else if (methodStr == state->m.ends_with) regs[base] = BblValue::makeBool(str->data.ends_with(args[0].stringVal()->data));
        else if (methodStr == state->m.upper) { std::string r = str->data; for (auto& c : r) c = static_cast<char>(toupper(c)); regs[base] = BblValue::makeString(state->intern(r)); }
        else if (methodStr == state->m.lower) { std::string r = str->data; for (auto& c : r) c = static_cast<char>(tolower(c)); regs[base] = BblValue::makeString(state->intern(r)); }
        else if (methodStr == state->m.trim) {
            auto s = str->data.find_first_not_of(" \t\n\r");
            auto e = str->data.find_last_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(s == std::string::npos ? "" : str->data.substr(s, e - s + 1)));
        } else if (methodStr == state->m.replace) {
            std::string from = args[0].stringVal()->data;
            if (from.empty()) JIT_ERROR(state, "string.replace: search string must not be empty");
            std::string result = str->data, to = args[1].stringVal()->data;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) { result.replace(pos, from.size(), to); pos += to.size(); }
            regs[base] = BblValue::makeString(state->intern(result));
        } else if (methodStr == state->m.join) {
            BblValue container = args[0];
            std::string result;
            if (container.type() == BBL::Type::Table) {
                BblTable* tbl = container.tableVal();
                for (int64_t i = 0; i < tbl->nextIntKey; i++) {
                    if (i > 0) result += str->data;
                    BblValue elem = tbl->get(BblValue::makeInt(i)).value_or(BblValue::makeNull());
                    result += jitValToStr(*state, elem);
                }
            }
            regs[base] = BblValue::makeString(state->intern(result));
        } else if (methodStr == state->m.split) {
            std::string delim = args[0].stringVal()->data;
            if (delim.empty()) JIT_ERROR(state, "string.split: separator must not be empty");
            BblTable* tbl = state->allocTable();
            size_t pos = 0; int64_t idx = 0;
            while (pos <= str->data.size()) {
                size_t next = str->data.find(delim, pos);
                if (next == std::string::npos) next = str->data.size();
                tbl->set(BblValue::makeInt(idx++), BblValue::makeString(state->intern(str->data.substr(pos, next - pos))));
                pos = next + delim.size();
                if (next == str->data.size()) break;
            }
            regs[base] = BblValue::makeTable(tbl);
        } else if (methodStr->data == "trim-left") {
            auto s = str->data.find_first_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(s == std::string::npos ? "" : str->data.substr(s)));
        } else if (methodStr->data == "trim-right") {
            auto e = str->data.find_last_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(e == std::string::npos ? "" : str->data.substr(0, e + 1)));
        } else if (methodStr->data == "pad-left") {
            int64_t width = args[0].intVal();
            char fill = argc > 1 ? args[1].stringVal()->data[0] : ' ';
            std::string r = str->data;
            if (static_cast<int64_t>(r.size()) < width) r.insert(0, static_cast<size_t>(width) - r.size(), fill);
            regs[base] = BblValue::makeString(state->intern(r));
        } else if (methodStr->data == "pad-right") {
            int64_t width = args[0].intVal();
            char fill = argc > 1 ? args[1].stringVal()->data[0] : ' ';
            std::string r = str->data;
            if (static_cast<int64_t>(r.size()) < width) r.append(static_cast<size_t>(width) - r.size(), fill);
            regs[base] = BblValue::makeString(state->intern(r));
        } else JIT_ERROR(state, "unknown string method: " + methodStr->data);
    } else if (receiver.type() == BBL::Type::Binary) {
        BblBinary* bin = receiver.binaryVal();
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(bin->length()));
        else if (methodStr == state->m.at) {
            int64_t idx = args[0].intVal();
            if (idx < 0 || static_cast<size_t>(idx) >= bin->length()) JIT_ERROR(state, "binary index out of bounds");
            regs[base] = BblValue::makeInt(bin->data[static_cast<size_t>(idx)]);
        } else if (methodStr == state->m.set) {
            int64_t idx = args[0].intVal();
            if (idx < 0 || static_cast<size_t>(idx) >= bin->length()) JIT_ERROR(state, "binary index out of bounds");
            if (args[1].type() != BBL::Type::Int) JIT_ERROR(state, "binary.set: value must be integer");
            bin->data[static_cast<size_t>(idx)] = static_cast<uint8_t>(args[1].intVal());
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.slice) {
            int64_t s = args[0].intVal(), l = args[1].intVal();
            if (s < 0 || l < 0 || static_cast<size_t>(s + l) > bin->length()) JIT_ERROR(state, "binary.slice: out of bounds");
            regs[base] = BblValue::makeBinary(state->allocBinary(std::vector<uint8_t>(bin->data.begin() + s, bin->data.begin() + s + l)));
        } else if (methodStr == state->m.resize) {
            int64_t sz = args[0].intVal();
            if (sz < 0) JIT_ERROR(state, "binary.resize: size must be non-negative");
            bin->data.resize(static_cast<size_t>(sz), 0);
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.copy_from) {
            if (args[0].type() != BBL::Type::Binary) JIT_ERROR(state, "binary.copy-from: source must be binary");
            BblBinary* src = args[0].binaryVal();
            int64_t dO = argc > 1 ? args[1].intVal() : 0;
            int64_t sO = argc > 2 ? args[2].intVal() : 0;
            int64_t ln = argc > 3 ? args[3].intVal() : static_cast<int64_t>(src->length()) - sO;
            if (dO < 0 || sO < 0 || ln < 0) JIT_ERROR(state, "binary.copy-from: negative offset");
            if (static_cast<size_t>(dO + ln) > bin->length()) JIT_ERROR(state, "binary.copy-from: destination overflow");
            if (static_cast<size_t>(sO + ln) > src->length()) JIT_ERROR(state, "binary.copy-from: source overflow");
            std::memcpy(bin->data.data() + dO, src->data.data() + sO, static_cast<size_t>(ln));
            regs[base] = BblValue::makeNull();
        } else JIT_ERROR(state, "unknown binary method: " + methodStr->data);
    } else if (receiver.type() == BBL::Type::UserData) {
        auto it = receiver.userdataVal()->desc->methods.find(methodStr->data);
        if (it == receiver.userdataVal()->desc->methods.end())
            JIT_ERROR(state, "unknown method '" + methodStr->data + "'");
        state->callArgs.clear();
        state->callArgs.push_back(receiver);
        for (int i = 0; i < argc; i++) state->callArgs.push_back(args[i]);
        state->hasReturn = false; state->returnValue = BblValue::makeNull();
        it->second(state);
        regs[base] = state->hasReturn ? state->returnValue : BblValue::makeNull();
    } else JIT_ERROR(state, "cannot call method on " + std::string(typeName(receiver.type())));
    } catch (const BBL::Error& e) {
        JIT_ERROR(state, e.what);
    } catch (const BblTerminated&) {
        JIT_ERROR(state, "terminated");
    } catch (const std::exception& e) {
        JIT_ERROR(state, e.what());
    }
}

void jitVector(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint32_t packed) {
    JIT_TRY
    uint8_t argc = (packed >> 8) & 0xFF;
    uint8_t typeIdx = packed & 0xFF;
    std::string elemType = chunk->constants[typeIdx].stringVal()->data;
    BBL::Type elemTypeTag = BBL::Type::Null;
    size_t elemSize = 0;
    auto dit = state->structDescs.find(elemType);
    if (dit != state->structDescs.end()) { elemTypeTag = BBL::Type::Struct; elemSize = dit->second.totalSize; }
    else if (elemType == "int" || elemType == "int64") { elemTypeTag = BBL::Type::Int; elemSize = 8; }
    else if (elemType == "float" || elemType == "float64") { elemTypeTag = BBL::Type::Float; elemSize = 8; }
    else if (elemType == "float32") { elemTypeTag = BBL::Type::Float; elemSize = 4; }
    else if (elemType == "int32") { elemTypeTag = BBL::Type::Int; elemSize = 4; }
    else JIT_ERROR(state, "unknown vector element type: " + elemType);
    BblVec* vec = state->allocVector(elemType, elemTypeTag, elemSize);
    if (argc == 1 && regs[destReg + 1].type() == BBL::Type::Binary) {
        BblBinary* bin = regs[destReg + 1].binaryVal();
        if (elemSize == 0 || bin->data.size() % elemSize != 0)
            JIT_ERROR(state, "vector: binary size " + std::to_string(bin->data.size()) +
                      " is not a multiple of element size " + std::to_string(elemSize));
        vec->data = bin->data;
    } else {
        for (int i = 0; i < argc; i++) state->packValue(vec, regs[destReg + 1 + i]);
    }
    regs[destReg] = BblValue::makeVector(vec);
    JIT_CATCH
}

void jitBinary(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    JIT_TRY
    BblValue& arg = regs[srcReg];
    if (arg.type() == BBL::Type::Vector) regs[destReg] = BblValue::makeBinary(state->allocBinary(arg.vectorVal()->data));
    else if (arg.type() == BBL::Type::Struct) regs[destReg] = BblValue::makeBinary(state->allocBinary(arg.structVal()->data));
    else if (arg.type() == BBL::Type::Int) regs[destReg] = BblValue::makeBinary(state->allocBinary(std::vector<uint8_t>(static_cast<size_t>(arg.intVal()), 0)));
    else JIT_ERROR(state, "binary: invalid argument type");
    JIT_CATCH
}

void jitLength(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    JIT_TRY
    BblValue& obj = regs[srcReg];
    if (obj.type() == BBL::Type::Vector) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.vectorVal()->length()));
    else if (obj.type() == BBL::Type::String) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.stringVal()->data.size()));
    else if (obj.type() == BBL::Type::Binary) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.binaryVal()->length()));
    else if (obj.type() == BBL::Type::Table) regs[destReg] = BblValue::makeInt(static_cast<int64_t>(obj.tableVal()->length()));
    else JIT_ERROR(state, "cannot get length");
    JIT_CATCH
}

void jitGetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint32_t packed) {
    JIT_TRY
    uint8_t objReg = (packed >> 8) & 0xFF;
    uint8_t nameIdx = packed & 0xFF;
    BblValue& obj = regs[objReg];
    std::string fieldName = chunk->constants[nameIdx].stringVal()->data;
    if (obj.type() == BBL::Type::Struct) {
        for (auto& fd : obj.structVal()->desc->fields)
            if (fd.name == fieldName) { regs[destReg] = state->readField(obj.structVal(), fd); return; }
        JIT_ERROR(state, "struct has no field '" + fieldName + "'");
    } else if (obj.type() == BBL::Type::Table) {
        regs[destReg] = obj.tableVal()->get(BblValue::makeString(state->intern(fieldName))).value_or(BblValue::makeNull());
    } else JIT_ERROR(state, "cannot access field");
    JIT_CATCH
}

void jitSetField(BblValue* regs, BblState* state, Chunk* chunk, uint8_t valReg, uint32_t packed) {
    JIT_TRY
    uint8_t objReg = (packed >> 8) & 0xFF;
    uint8_t nameIdx = packed & 0xFF;
    BblValue& obj = regs[objReg];
    std::string fieldName = chunk->constants[nameIdx].stringVal()->data;
    if (obj.type() == BBL::Type::Struct) {
        for (auto& fd : obj.structVal()->desc->fields)
            if (fd.name == fieldName) { state->writeField(obj.structVal(), fd, regs[valReg]); return; }
    } else if (obj.type() == BBL::Type::Table) {
        obj.tableVal()->set(BblValue::makeString(state->intern(fieldName)), regs[valReg]);
    }
    JIT_CATCH
}

void jitGetIndex(BblValue* regs, BblState* state, uint32_t packed, uint32_t unused) {
    JIT_TRY
    (void)unused;
    uint8_t destReg = (packed >> 16) & 0xFF;
    uint8_t objReg = (packed >> 8) & 0xFF;
    uint8_t idxReg = packed & 0xFF;
    BblValue& obj = regs[objReg]; BblValue& idx = regs[idxReg];
    if (obj.type() == BBL::Type::Vector) regs[destReg] = state->readVecElem(obj.vectorVal(), static_cast<size_t>(idx.intVal()));
    else if (obj.type() == BBL::Type::Table) regs[destReg] = obj.tableVal()->get(idx).value_or(BblValue::makeNull());
    else JIT_ERROR(state, "cannot index");
    JIT_CATCH
}

void jitSetIndex(BblValue* regs, BblState* state, uint32_t packed, uint32_t unused) {
    JIT_TRY
    (void)unused;
    uint8_t valReg = (packed >> 16) & 0xFF;
    uint8_t objReg = (packed >> 8) & 0xFF;
    uint8_t idxReg = packed & 0xFF;
    BblValue& obj = regs[objReg]; BblValue& idx = regs[idxReg];
    if (obj.type() == BBL::Type::Vector) state->writeVecElem(obj.vectorVal(), static_cast<size_t>(idx.intVal()), regs[valReg]);
    else if (obj.type() == BBL::Type::Table) obj.tableVal()->set(idx, regs[valReg]);
    JIT_CATCH
}

void jitExec(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    JIT_TRY
    if (regs[srcReg].type() != BBL::Type::String) JIT_ERROR(state, "exec: argument must be string");
    BblLexer lexer(regs[srcReg].stringVal()->data.c_str());
    auto nodes = parse(lexer);
    BblValue result = BblValue::makeNull();
    for (auto& n : nodes) result = state->eval(n, state->rootScope);
    regs[destReg] = result;
    JIT_CATCH
}

void jitNot(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    (void)state;
    BblValue& v = regs[srcReg];
    bool falsy = (v.type() == BBL::Type::Null) ||
                 (v.type() == BBL::Type::Bool && !v.boolVal()) ||
                 (v.type() == BBL::Type::Int && v.intVal() == 0);
    regs[destReg] = BblValue::makeBool(falsy);
}

void jitStruct(BblValue* regs, BblState* state, Chunk* chunk, uint8_t A, uint32_t packed) {
    JIT_TRY
    uint8_t argc = (packed >> 8) & 0xFF;
    uint8_t typeIdx = packed & 0xFF;
    std::string tname = chunk->constants[typeIdx].stringVal()->data;
    auto dit = state->structDescs.find(tname);
    if (dit == state->structDescs.end()) JIT_ERROR(state, "unknown struct type: " + tname);
    std::vector<BblValue> args(argc);
    for (int i = 0; i < argc; i++) args[i] = regs[A + 1 + i];
    regs[A] = state->constructStruct(&dit->second, args, 0);
    JIT_CATCH
}

void jitSizeof(BblValue* regs, BblState* state, Chunk* chunk, uint8_t A, uint8_t constIdx) {
    JIT_TRY
    std::string tname = chunk->constants[constIdx].stringVal()->data;
    auto dit = state->structDescs.find(tname);
    if (dit != state->structDescs.end()) {
        regs[A] = BblValue::makeInt(static_cast<int64_t>(dit->second.totalSize));
        return;
    }
    uint32_t symId = state->resolveSymbol(tname);
    if (state->vm) {
        auto git = state->vm->globals.find(symId);
        if (git != state->vm->globals.end() && git->second.type() == BBL::Type::Struct) {
            regs[A] = BblValue::makeInt(static_cast<int64_t>(git->second.structVal()->desc->totalSize));
            return;
        }
    }
    JIT_ERROR(state, "sizeof: unknown type or variable: " + tname);
    JIT_CATCH
}

void jitExecFile(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    JIT_TRY
    if (regs[srcReg].type() != BBL::Type::String) JIT_ERROR(state, "execfile: argument must be string");
    state->execfile(regs[srcReg].stringVal()->data);
    regs[destReg] = BblValue::makeNull();
    JIT_CATCH
}

void jitStoreError(BblValue* regs, BblState* state, uint8_t destReg, uint8_t unused) {
    (void)unused;
    regs[destReg] = BblValue::makeString(state->intern(g_jitErrorMsg));
    g_jitError = false;
    g_jitErrorMsg.clear();
}

static std::string jitValToStr(BblState& state, const BblValue& v) {
    switch (v.type()) {
        case BBL::Type::Null: return "null";
        case BBL::Type::Bool: return v.boolVal() ? "true" : "false";
        case BBL::Type::Int: return std::to_string(v.intVal());
        case BBL::Type::Float: { char b[64]; snprintf(b, 64, "%g", v.floatVal()); return b; }
        case BBL::Type::String: return v.stringVal()->data;
        default: return "<object>";
    }
}

// op: 0=ADD, 1=SUB, 2=MUL, 3=DIV, 4=MOD
void jitArith(BblValue* regs, BblState* state, uint8_t A, uint32_t packed) {
    uint8_t op = (packed >> 16) & 0xFF;
    uint8_t B = (packed >> 8) & 0xFF;
    uint8_t C = packed & 0xFF;
    BblValue& rb = regs[B]; BblValue& rc = regs[C];
    auto toF = [](const BblValue& v) -> double {
        if (v.type() == BBL::Type::Int) return static_cast<double>(v.intVal());
        return v.floatVal();
    };
    auto isNum = [](const BblValue& v) -> bool {
        return v.type() == BBL::Type::Int || v.type() == BBL::Type::Float || v.isDouble();
    };
    if (op == 0) {
        if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int)
            regs[A] = BblValue::makeInt(rb.intVal() + rc.intVal());
        else if (rb.type() == BBL::Type::String) {
            regs[A] = BblValue::makeString(state->allocString(rb.stringVal()->data + jitValToStr(*state, rc)));
        } else if (isNum(rb) && isNum(rc))
            regs[A] = BblValue::makeFloat(toF(rb) + toF(rc));
        else
            JIT_ERROR(state, "type mismatch in addition");
    } else if (op == 1) {
        if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int)
            regs[A] = BblValue::makeInt(rb.intVal() - rc.intVal());
        else
            regs[A] = BblValue::makeFloat(toF(rb) - toF(rc));
    } else if (op == 2) {
        if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int)
            regs[A] = BblValue::makeInt(rb.intVal() * rc.intVal());
        else
            regs[A] = BblValue::makeFloat(toF(rb) * toF(rc));
    } else if (op == 3) {
        if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) {
            if (rc.intVal() == 0) JIT_ERROR(state, "division by zero");
            regs[A] = BblValue::makeInt(rb.intVal() / rc.intVal());
        } else {
            double denom = toF(rc);
            if (denom == 0.0) JIT_ERROR(state, "division by zero");
            regs[A] = BblValue::makeFloat(toF(rb) / denom);
        }
    } else {
        if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) {
            if (rc.intVal() == 0) JIT_ERROR(state, "division by zero");
            regs[A] = BblValue::makeInt(rb.intVal() % rc.intVal());
        } else
            regs[A] = BblValue::makeFloat(std::fmod(toF(rb), toF(rc)));
    }
}

void jitBitwise(BblValue* regs, BblState* state, uint8_t A, uint32_t packed) {
    uint8_t op = (packed >> 16) & 0xFF;
    uint8_t B = (packed >> 8) & 0xFF;
    uint8_t C = packed & 0xFF;
    if (regs[B].type() != BBL::Type::Int) JIT_ERROR(state, "bitwise ops require integers");
    if (op != 3 && regs[C].type() != BBL::Type::Int) JIT_ERROR(state, "bitwise ops require integers");
    int64_t a = regs[B].intVal();
    if (op == 0) regs[A] = BblValue::makeInt(a & regs[C].intVal());
    else if (op == 1) regs[A] = BblValue::makeInt(a | regs[C].intVal());
    else if (op == 2) regs[A] = BblValue::makeInt(a ^ regs[C].intVal());
    else if (op == 3) regs[A] = BblValue::makeInt(~a);
    else if (op == 4) {
        int64_t shift = regs[C].intVal();
        if (shift < 0) JIT_ERROR(state, "negative shift count");
        regs[A] = BblValue::makeInt(shift >= 64 ? 0 : (a << shift));
    }
    else if (op == 5) {
        int64_t shift = regs[C].intVal();
        if (shift < 0) JIT_ERROR(state, "negative shift count");
        regs[A] = BblValue::makeInt(shift >= 64 ? (a >> 63) : (a >> shift));
    }
}

static void emit(uint8_t* buf, size_t& pos, const void* data, size_t len) {
    std::memcpy(buf + pos, data, len);
    pos += len;
}

static void emit8(uint8_t* buf, size_t& pos, uint8_t v) { buf[pos++] = v; }

static void emit32(uint8_t* buf, size_t& pos, uint32_t v) {
    std::memcpy(buf + pos, &v, 4);
    pos += 4;
}

static void emit64(uint8_t* buf, size_t& pos, uint64_t v) {
    std::memcpy(buf + pos, &v, 8);
    pos += 8;
}

static void patchRel32(uint8_t* buf, size_t patchOffset, size_t target) {
    int32_t rel = static_cast<int32_t>(target - (patchOffset + 4));
    std::memcpy(buf + patchOffset, &rel, 4);
}

// shl rax, 16; sar rax, 16  — sign-extend 48-bit payload to int64
static void emitUnboxIntRax(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x48, 0xC1, 0xE0, 0x10,
        0x48, 0xC1, 0xF8, 0x10,
    };
    emit(buf, pos, code, sizeof(code));
}

// shl rcx, 16; sar rcx, 16
static void emitUnboxIntRcx(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x48, 0xC1, 0xE1, 0x10,
        0x48, 0xC1, 0xF9, 0x10,
    };
    emit(buf, pos, code, sizeof(code));
}

// and rax, NB_PAYLOAD; or rax, NB_TAG_INT; mov [rbx+off], rax
static void emitReboxIntStore(uint8_t* buf, size_t& pos, int off) {
    uint8_t movabs[] = { 0x48, 0xB9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_PAYLOAD);
    uint8_t andOp[] = { 0x48, 0x21, 0xC8 };
    emit(buf, pos, andOp, 3);
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    uint8_t orOp[] = { 0x48, 0x09, 0xC8 };
    emit(buf, pos, orOp, 3);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, off);
}

// Prologue: push rbx, r12, r13; mov rbx,rdi; mov r12,rsi; mov r13,rdx
static void emitPrologue(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x53,                   // push rbx
        0x41, 0x54,             // push r12
        0x41, 0x55,             // push r13
        0x48, 0x89, 0xfb,       // mov rbx, rdi  (regs)
        0x49, 0x89, 0xf4,       // mov r12, rsi  (state)
        0x49, 0x89, 0xd5,       // mov r13, rdx  (chunk)
    };
    emit(buf, pos, code, sizeof(code));
}

// Epilogue: pop r13, r12, rbx; ret
static void emitEpilogue(uint8_t* buf, size_t& pos) {
    uint8_t code[] = {
        0x41, 0x5d,             // pop r13
        0x41, 0x5c,             // pop r12
        0x5b,                   // pop rbx
        0xc3,                   // ret
    };
    emit(buf, pos, code, sizeof(code));
}

// RETURN R[A]: load NaN-boxed value into rax, then epilogue
static void emitReturn(uint8_t* buf, size_t& pos, int A) {
    // mov rax, [rbx + A*8]
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, A * VAL_SIZE);

    emitEpilogue(buf, pos);
}

// ADD R[A] = R[B] + R[C] (integer, NaN-boxed — direct arithmetic, no unbox)
static void emitAdd(uint8_t* buf, size_t& pos, int A, int B, int C) {
    // mov rax, [rbx + B*8]
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    // add rax, [rbx + C*8]
    uint8_t add[] = { 0x48, 0x03, 0x83 };
    emit(buf, pos, add, 3);
    emit32(buf, pos, C * VAL_SIZE);
    // movabs rcx, NB_PAYLOAD; and rax, rcx
    uint8_t movabs[] = { 0x48, 0xB9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_PAYLOAD);
    uint8_t andOp[] = { 0x48, 0x21, 0xC8 };
    emit(buf, pos, andOp, 3);
    // movabs rcx, NB_TAG_INT; or rax, rcx
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    uint8_t orOp[] = { 0x48, 0x09, 0xC8 };
    emit(buf, pos, orOp, 3);
    // mov [rbx + A*8], rax
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// SUB R[A] = R[B] - R[C] (integer, NaN-boxed — direct arithmetic, no unbox)
static void emitSub(uint8_t* buf, size_t& pos, int A, int B, int C) {
    // mov rax, [rbx + B*8]
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    // sub rax, [rbx + C*8]
    uint8_t sub[] = { 0x48, 0x2b, 0x83 };
    emit(buf, pos, sub, 3);
    emit32(buf, pos, C * VAL_SIZE);
    // movabs rcx, NB_PAYLOAD; and rax, rcx
    uint8_t movabs[] = { 0x48, 0xB9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_PAYLOAD);
    uint8_t andOp[] = { 0x48, 0x21, 0xC8 };
    emit(buf, pos, andOp, 3);
    // movabs rcx, NB_TAG_INT; or rax, rcx
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    uint8_t orOp[] = { 0x48, 0x09, 0xC8 };
    emit(buf, pos, orOp, 3);
    // mov [rbx + A*8], rax
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// MUL R[A] = R[B] * R[C]
static void emitMul(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE);
    emitUnboxIntRax(buf, pos);
    uint8_t mov2[] = { 0x48, 0x8b, 0x8b };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, C * VAL_SIZE);
    emitUnboxIntRcx(buf, pos);
    uint8_t imul[] = { 0x48, 0x0f, 0xaf, 0xc1 };
    emit(buf, pos, imul, 4);
    emitReboxIntStore(buf, pos, A * VAL_SIZE);
}

// ADDI R[A] += imm (NaN-boxed)
static void emitAddi(uint8_t* buf, size_t& pos, int A, int imm) {
    if (imm >= 0 && imm <= 32767) {
        // add qword [rbx + A*8], imm32
        uint8_t addMem[] = { 0x48, 0x81, 0x83 };
        emit(buf, pos, addMem, 3);
        emit32(buf, pos, A * VAL_SIZE);
        emit32(buf, pos, static_cast<uint32_t>(imm));
    } else {
        uint8_t load[] = { 0x48, 0x8b, 0x83 };
        emit(buf, pos, load, 3);
        emit32(buf, pos, A * VAL_SIZE);
        emitUnboxIntRax(buf, pos);
        uint8_t add[] = { 0x48, 0x05 };
        emit(buf, pos, add, 2);
        emit32(buf, pos, static_cast<uint32_t>(imm));
        emitReboxIntStore(buf, pos, A * VAL_SIZE);
    }
}

// SUBI R[A] -= imm (NaN-boxed)
static void emitSubi(uint8_t* buf, size_t& pos, int A, int imm) {
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, A * VAL_SIZE);
    emitUnboxIntRax(buf, pos);
    // sub rax, imm32
    uint8_t sub[] = { 0x48, 0x2d };
    emit(buf, pos, sub, 2);
    emit32(buf, pos, static_cast<uint32_t>(imm));
    emitReboxIntStore(buf, pos, A * VAL_SIZE);
}

// LOADINT R[A] = NaN-boxed int
static void emitLoadInt(uint8_t* buf, size_t& pos, int A, int imm) {
    uint64_t val = NB_TAG_INT | (static_cast<uint64_t>(static_cast<int64_t>(imm)) & NB_PAYLOAD);
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, val);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// LOADK R[A] = *constPtr (8-byte copy via pointer)
static void emitLoadK(uint8_t* buf, size_t& pos, int A, BblValue* constPtr) {
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(constPtr));
    // mov rax, [rax]
    uint8_t load[] = { 0x48, 0x8b, 0x00 };
    emit(buf, pos, load, 3);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// LOADNULL R[A] = NB_TAG_NULL
static void emitLoadNull(uint8_t* buf, size_t& pos, int A) {
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_NULL);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// MOVE R[A] = R[B] (8-byte copy)
static void emitMove(uint8_t* buf, size_t& pos, int A, int B) {
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

// Compare-and-jump: unbox ints from A and B, cmp, conditional jump
static size_t emitCompareJmp(uint8_t* buf, size_t& pos, int A, int B, uint8_t jccByte) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, A * VAL_SIZE);
    emitUnboxIntRax(buf, pos);
    uint8_t mov2[] = { 0x48, 0x8b, 0x8b };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, B * VAL_SIZE);
    emitUnboxIntRcx(buf, pos);
    uint8_t cmp[] = { 0x48, 0x39, 0xc8 };
    emit(buf, pos, cmp, 3);
    uint8_t jcc[] = { 0x0f, jccByte };
    emit(buf, pos, jcc, 2);
    size_t p = pos;
    emit32(buf, pos, 0);
    return p;
}

static size_t emitLtjmp(uint8_t* buf, size_t& pos, int A, int B) {
    return emitCompareJmp(buf, pos, A, B, 0x8c);
}

static size_t emitLejmp(uint8_t* buf, size_t& pos, int A, int B) {
    return emitCompareJmp(buf, pos, A, B, 0x8e);
}

static size_t emitGtjmp(uint8_t* buf, size_t& pos, int A, int B) {
    return emitCompareJmp(buf, pos, A, B, 0x8f);
}

static size_t emitGejmp(uint8_t* buf, size_t& pos, int A, int B) {
    return emitCompareJmp(buf, pos, A, B, 0x8d);
}

// JMP / LOOP: unconditional jump
static size_t emitJmp(uint8_t* buf, size_t& pos) {
    emit8(buf, pos, 0xe9); // jmp rel32
    size_t p = pos;
    emit32(buf, pos, 0);
    return p;
}

// Emit a call to a C function: movabs rax, <addr>; call rax
// Before calling, set up args: rdi=rbx(regs), rsi=r12(state)
// Additional args in edx, ecx, r8d per System V ABI
static void emitErrorCheck(uint8_t* buf, size_t& pos, std::vector<size_t>& errorExitPatches) {
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(&g_jitError));
    uint8_t cmpb[] = { 0x80, 0x38, 0x00 };
    emit(buf, pos, cmpb, 3);
    uint8_t jne[] = { 0x0f, 0x85 };
    emit(buf, pos, jne, 2);
    errorExitPatches.push_back(pos);
    emit32(buf, pos, 0);
}

static void emitCallHelper2(uint8_t* buf, size_t& pos, void* fn, uint32_t arg3, uint32_t arg4, std::vector<size_t>* errPatches = nullptr) {
    uint8_t a1[] = { 0x48, 0x89, 0xdf };
    emit(buf, pos, a1, 3);
    uint8_t a2[] = { 0x4c, 0x89, 0xe6 };
    emit(buf, pos, a2, 3);
    uint8_t a3[] = { 0xba };
    emit(buf, pos, a3, 1);
    emit32(buf, pos, arg3);
    uint8_t a4[] = { 0xb9 };
    emit(buf, pos, a4, 1);
    emit32(buf, pos, arg4);
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(fn));
    uint8_t call[] = { 0xff, 0xd0 };
    emit(buf, pos, call, 2);
    if (errPatches) emitErrorCheck(buf, pos, *errPatches);
}

static void emitCallHelper3(uint8_t* buf, size_t& pos, void* fn, uint32_t arg3, uint32_t arg4, uint32_t arg5, std::vector<size_t>* errPatches = nullptr) {
    uint8_t a1[] = { 0x48, 0x89, 0xdf };
    emit(buf, pos, a1, 3);
    uint8_t a2[] = { 0x4c, 0x89, 0xe6 };
    emit(buf, pos, a2, 3);
    uint8_t a3[] = { 0x4c, 0x89, 0xea };
    emit(buf, pos, a3, 3);
    uint8_t a4[] = { 0xb9 };
    emit(buf, pos, a4, 1);
    emit32(buf, pos, arg4);
    uint8_t a5[] = { 0x41, 0xb8 };
    emit(buf, pos, a5, 2);
    emit32(buf, pos, arg5);
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>(fn));
    uint8_t call[] = { 0xff, 0xd0 };
    emit(buf, pos, call, 2);
    if (errPatches) emitErrorCheck(buf, pos, *errPatches);
}

// isFalsy check for NaN-boxed values: falsy if bits == TAG_NULL, TAG_BOOL|0, or TAG_INT|0
static size_t emitJmpFalse(uint8_t* buf, size_t& pos, int A) {
    // mov rax, [rbx + A*8]
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, A * VAL_SIZE);

    uint8_t movabs[] = { 0x48, 0xb9 };
    uint8_t cmp[] = { 0x48, 0x39, 0xc8 };
    uint8_t je[] = { 0x0f, 0x84 };

    // cmp rax, NB_TAG_NULL; je falsy
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_NULL);
    emit(buf, pos, cmp, 3);
    emit(buf, pos, je, 2);
    size_t p1 = pos;
    emit32(buf, pos, 0);

    // cmp rax, NB_TAG_BOOL; je falsy (bool false = TAG_BOOL | 0)
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_BOOL);
    emit(buf, pos, cmp, 3);
    emit(buf, pos, je, 2);
    size_t p2 = pos;
    emit32(buf, pos, 0);

    // cmp rax, NB_TAG_INT; je falsy (int 0 = TAG_INT | 0)
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    emit(buf, pos, cmp, 3);
    emit(buf, pos, je, 2);
    size_t p3 = pos;
    emit32(buf, pos, 0);

    // not falsy — skip over
    uint8_t jmpShort[] = { 0xeb };
    emit(buf, pos, jmpShort, 1);
    size_t skipOff = pos;
    emit8(buf, pos, 0);

    // falsy_label:
    size_t falsyLabel = pos;
    emit8(buf, pos, 0xe9);
    size_t targetPatch = pos;
    emit32(buf, pos, 0);

    patchRel32(buf, p1, falsyLabel);
    patchRel32(buf, p2, falsyLabel);
    patchRel32(buf, p3, falsyLabel);
    buf[skipOff] = static_cast<uint8_t>(pos - skipOff - 1);

    return targetPatch;
}

// Comparisons: R[A] = (R[B] <op> R[C]) as NaN-boxed bool
static void emitCmp(uint8_t* buf, size_t& pos, int A, int B, int C, uint8_t jccOpcode) {
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE);
    emitUnboxIntRax(buf, pos);
    uint8_t mov2[] = { 0x48, 0x8b, 0x8b };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, C * VAL_SIZE);
    emitUnboxIntRcx(buf, pos);
    // cmp rax, rcx
    uint8_t cmp[] = { 0x48, 0x39, 0xc8 };
    emit(buf, pos, cmp, 3);
    // setCC al
    uint8_t setcc[] = { 0x0f, jccOpcode, 0xc0 };
    emit(buf, pos, setcc, 3);
    // movzx eax, al
    uint8_t movzx[] = { 0x0f, 0xb6, 0xc0 };
    emit(buf, pos, movzx, 3);
    // movabs rcx, NB_TAG_BOOL; or rax, rcx; store
    uint8_t movabs[] = { 0x48, 0xb9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_BOOL);
    uint8_t orOp[] = { 0x48, 0x09, 0xc8 };
    emit(buf, pos, orOp, 3);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

struct JumpPatch {
    size_t patchOffset;  // native byte offset of rel32 to patch
    size_t targetInst;   // bytecode instruction index to jump to
};

JitCode jitCompile(BblState& state, Chunk& chunk, BblClosure* self) {
    JitCode jit;
    jit.capacity = chunk.code.size() * 96 + 1024;
    jit.buf = static_cast<uint8_t*>(
        mmap(nullptr, jit.capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    jit.size = 0;

    emitPrologue(jit.buf, jit.size);

    std::vector<size_t> nativeOffsets(chunk.code.size() + 1, 0);
    std::vector<JumpPatch> patches;
    std::unordered_map<uint8_t, BblClosure*> closureRegs;
    std::set<uint8_t> selfRefRegs;
    std::vector<size_t> selfCallPatches;
    std::vector<size_t> errorExitPatches;

    struct TryInfo { int catchTarget; std::vector<size_t> outerPatches; uint8_t destReg; };
    std::vector<TryInfo> tryStack;
    struct TryCatchPatch { size_t patchOffset; size_t catchInst; uint8_t destReg; };
    std::vector<TryCatchPatch> tryCatchPatches;

    for (size_t i = 0; i < chunk.code.size(); i++) {
        nativeOffsets[i] = jit.size;
        uint32_t inst = chunk.code[i];
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst), B = decodeB(inst), C = decodeC(inst);
        uint16_t Bx = decodeBx(inst);
        int sBx = decodesBx(inst);

        if (op != OP_GETGLOBAL && op != OP_CLOSURE && op != OP_CALL) {
            selfRefRegs.erase(A);
            closureRegs.erase(A);
        }

        switch (op) {
        case OP_LOADK:
            emitLoadK(jit.buf, jit.size, A, &chunk.constants[Bx]);
            break;
        case OP_LOADINT:
            emitLoadInt(jit.buf, jit.size, A, sBx);
            break;
        case OP_LOADNULL:
            emitLoadNull(jit.buf, jit.size, A);
            break;
        case OP_LOADBOOL: {
            uint64_t val = NB_TAG_BOOL | (B ? 1ULL : 0ULL);
            uint8_t movabs[] = { 0x48, 0xb8 };
            emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, val);
            uint8_t store[] = { 0x48, 0x89, 0x83 };
            emit(jit.buf, jit.size, store, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE);
            break;
        }
        case OP_MOVE:
            emitMove(jit.buf, jit.size, A, B);
            break;
        case OP_ADD: {
            // Type guard: if R[B] and R[C] are both int, fast path
            // Check B
            uint8_t ld[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, ld, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE);
            uint8_t movabs1[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs1, 2);
            emit64(jit.buf, jit.size, BblValue::TAG_MASK);
            uint8_t andrr[] = { 0x48, 0x21, 0xc1 };
            emit(jit.buf, jit.size, andrr, 3);
            uint8_t movabs2[] = { 0x48, 0xba };
            emit(jit.buf, jit.size, movabs2, 2);
            emit64(jit.buf, jit.size, NB_TAG_INT);
            uint8_t cmprr[] = { 0x48, 0x39, 0xd1 };
            emit(jit.buf, jit.size, cmprr, 3);
            uint8_t jne[] = { 0x0f, 0x85 };
            emit(jit.buf, jit.size, jne, 2);
            size_t slowPatch1 = jit.size;
            emit32(jit.buf, jit.size, 0);
            // Check C
            emit(jit.buf, jit.size, ld, 3);
            emit32(jit.buf, jit.size, C * VAL_SIZE);
            emit(jit.buf, jit.size, movabs1, 2);
            emit64(jit.buf, jit.size, BblValue::TAG_MASK);
            emit(jit.buf, jit.size, andrr, 3);
            emit(jit.buf, jit.size, cmprr, 3);
            emit(jit.buf, jit.size, jne, 2);
            size_t slowPatch2 = jit.size;
            emit32(jit.buf, jit.size, 0);
            emitAdd(jit.buf, jit.size, A, B, C);
            emit8(jit.buf, jit.size, 0xe9);
            size_t donePatch = jit.size;
            emit32(jit.buf, jit.size, 0);
            patchRel32(jit.buf, slowPatch1, jit.size);
            patchRel32(jit.buf, slowPatch2, jit.size);
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((0 << 16) | (B << 8) | C), &errorExitPatches);
            patchRel32(jit.buf, donePatch, jit.size);
            break;
        }
        case OP_ADDI: {
            // Type guard: if R[A] is int, fast path
            uint8_t ld[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, ld, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE);
            uint8_t movabs1[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs1, 2);
            emit64(jit.buf, jit.size, BblValue::TAG_MASK);
            uint8_t andrr[] = { 0x48, 0x21, 0xc1 };
            emit(jit.buf, jit.size, andrr, 3);
            uint8_t movabs2[] = { 0x48, 0xba };
            emit(jit.buf, jit.size, movabs2, 2);
            emit64(jit.buf, jit.size, NB_TAG_INT);
            uint8_t cmprr[] = { 0x48, 0x39, 0xd1 };
            emit(jit.buf, jit.size, cmprr, 3);
            uint8_t jne[] = { 0x0f, 0x85 };
            emit(jit.buf, jit.size, jne, 2);
            size_t slowPatch = jit.size;
            emit32(jit.buf, jit.size, 0);
            emitAddi(jit.buf, jit.size, A, sBx);
            emit8(jit.buf, jit.size, 0xe9);
            size_t donePatch = jit.size;
            emit32(jit.buf, jit.size, 0);
            patchRel32(jit.buf, slowPatch, jit.size);
            // Slow: synthesize ADD R[A] = R[A] + imm via helper
            // Store imm in a temp register, then call jitArith
            emitLoadInt(jit.buf, jit.size, A + 1 < 255 ? A + 1 : A, sBx);
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A,
                static_cast<uint32_t>((0 << 16) | (A << 8) | (A + 1 < 255 ? A + 1 : A)), &errorExitPatches);
            patchRel32(jit.buf, donePatch, jit.size);
            break;
        }
        case OP_ADDK: {
            int64_t cv = chunk.constants[C].intVal();
            // Unbox B
            uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, mov1, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE);
            emitUnboxIntRax(jit.buf, jit.size);
            // movabs rcx, cv; add rax, rcx
            uint8_t movabs[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, static_cast<uint64_t>(cv));
            uint8_t addrr[] = { 0x48, 0x01, 0xc8 };
            emit(jit.buf, jit.size, addrr, 3);
            emitReboxIntStore(jit.buf, jit.size, A * VAL_SIZE);
            break;
        }
        case OP_SUB: {
            uint8_t ld[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, ld, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE);
            uint8_t movabs1[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs1, 2);
            emit64(jit.buf, jit.size, BblValue::TAG_MASK);
            uint8_t andrr[] = { 0x48, 0x21, 0xc1 };
            emit(jit.buf, jit.size, andrr, 3);
            uint8_t movabs2[] = { 0x48, 0xba };
            emit(jit.buf, jit.size, movabs2, 2);
            emit64(jit.buf, jit.size, NB_TAG_INT);
            uint8_t cmprr[] = { 0x48, 0x39, 0xd1 };
            emit(jit.buf, jit.size, cmprr, 3);
            uint8_t jne[] = { 0x0f, 0x85 };
            emit(jit.buf, jit.size, jne, 2);
            size_t slowPatch1 = jit.size;
            emit32(jit.buf, jit.size, 0);
            emit(jit.buf, jit.size, ld, 3);
            emit32(jit.buf, jit.size, C * VAL_SIZE);
            emit(jit.buf, jit.size, movabs1, 2);
            emit64(jit.buf, jit.size, BblValue::TAG_MASK);
            emit(jit.buf, jit.size, andrr, 3);
            emit(jit.buf, jit.size, cmprr, 3);
            emit(jit.buf, jit.size, jne, 2);
            size_t slowPatch2 = jit.size;
            emit32(jit.buf, jit.size, 0);
            emitSub(jit.buf, jit.size, A, B, C);
            emit8(jit.buf, jit.size, 0xe9);
            size_t donePatch = jit.size;
            emit32(jit.buf, jit.size, 0);
            patchRel32(jit.buf, slowPatch1, jit.size);
            patchRel32(jit.buf, slowPatch2, jit.size);
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((1 << 16) | (B << 8) | C), &errorExitPatches);
            patchRel32(jit.buf, donePatch, jit.size);
            break;
        }
        case OP_SUBI:
            emitSubi(jit.buf, jit.size, A, sBx);
            break;
        case OP_MUL:
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((2 << 16) | (B << 8) | C), &errorExitPatches);
            break;

        case OP_LTJMP: {
            size_t p = emitLtjmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2}); // skip next instruction (the JMP)
            break;
        }
        case OP_LEJMP: {
            size_t p = emitLejmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }
        case OP_GTJMP: {
            size_t p = emitGtjmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }
        case OP_GEJMP: {
            size_t p = emitGejmp(jit.buf, jit.size, A, B);
            patches.push_back({p, i + 2});
            break;
        }

        case OP_JMP: {
            size_t p = emitJmp(jit.buf, jit.size);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_LOOP: {
            // Backward jump — target is already compiled
            size_t p = emitJmp(jit.buf, jit.size);
            int target = static_cast<int>(i) + 1 - sBx;
            patchRel32(jit.buf, p, nativeOffsets[target]);
            break;
        }

        case OP_RETURN:
            emitReturn(jit.buf, jit.size, A);
            break;

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            if (self && state.vm) {
                auto it = state.vm->globals.find(symId);
                if (it != state.vm->globals.end() &&
                    it->second.type() == BBL::Type::Fn &&
                    it->second.isClosure() &&
                    it->second.closureVal() == self) {
                    selfRefRegs.insert(A);
                }
            }
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A, &errorExitPatches);
            break;
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetGlobal, symId, A, &errorExitPatches);
            break;
        }
        case OP_CALL: {
            if (selfRefRegs.count(A)) {
                // Native self-recursive call through full prologue
                // lea rdi, [rbx + A*16]
                uint8_t lea[] = { 0x48, 0x8d, 0xbb };
                emit(jit.buf, jit.size, lea, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                // mov rsi, r12 (state)
                uint8_t movsi[] = { 0x4c, 0x89, 0xe6 };
                emit(jit.buf, jit.size, movsi, 3);
                // mov rdx, r13 (chunk)
                uint8_t movdx[] = { 0x4c, 0x89, 0xea };
                emit(jit.buf, jit.size, movdx, 3);
                // call rel32 → function entry (offset 0)
                emit8(jit.buf, jit.size, 0xe8);
                selfCallPatches.push_back(jit.size);
                emit32(jit.buf, jit.size, 0);
                // result in rax (NaN-boxed BblValue)
                uint8_t st1[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, st1, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
            } else {
            // Try to inline: check if base register holds a known closure
            auto cit = closureRegs.find(A);
            if (cit != closureRegs.end() && B == cit->second->arity) {
                BblClosure* callee = cit->second;
                // Inline the callee's bytecode with register remapping:
                // Callee R[0] = callee (unused), R[1..arity] = args from caller R[A+1..A+argc]
                // Callee's other regs mapped to caller regs above A+argc
                uint8_t argBase = A + 1; // caller regs holding args
                // For each callee instruction, remap registers
                for (size_t ci = 0; ci < callee->chunk.code.size(); ci++) {
                    uint32_t cinst = callee->chunk.code[ci];
                    uint8_t cop = decodeOP(cinst);
                    uint8_t cA = decodeA(cinst), cB = decodeB(cinst), cC = decodeC(cinst);
                    int csBx = decodesBx(cinst);

                    // Remap register: callee R[0] → skip, R[1..arity] → caller R[A+1..A+arity]
                    // R[arity+1..] → caller R[A+arity+1..]
                    auto remap = [&](uint8_t r) -> uint8_t {
                        if (r == 0) return A; // callee slot (unused in practice)
                        if (r <= callee->arity) return A + r; // param → arg register
                        return A + r; // temp → above args
                    };

                    switch (cop) {
                    case OP_ADD: emitAdd(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_SUB: emitSub(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_MUL: emitMul(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); break;
                    case OP_ADDI: emitAddi(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_SUBI: emitSubi(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_MOVE: emitMove(jit.buf, jit.size, remap(cA), remap(cB)); break;
                    case OP_LOADINT: emitLoadInt(jit.buf, jit.size, remap(cA), csBx); break;
                    case OP_LOADK: emitLoadK(jit.buf, jit.size, remap(cA), &callee->chunk.constants[decodeBx(cinst)]); break;
                    case OP_RETURN:
                        // Inline return: move callee result to caller's base register
                        if (remap(cA) != A) emitMove(jit.buf, jit.size, A, remap(cA));
                        goto inline_done;
                    default:
                        // Can't inline this instruction — fall back to helper call
                        emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B, &errorExitPatches);
                        goto inline_done;
                    }
                }
                inline_done:;
            } else {
                emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B, &errorExitPatches);
            }
            } // end self-ref else
            break;
        }
        case OP_CLOSURE:
            emitCallHelper3(jit.buf, jit.size, (void*)jitClosure, 0, A, Bx, &errorExitPatches);
            // Track that register A now holds this closure proto
            closureRegs[A] = chunk.constants[Bx].closureVal();
            break;

        case OP_JMPFALSE: {
            size_t p = emitJmpFalse(jit.buf, jit.size, A);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_JMPTRUE: {
            // Invert: emit jmpfalse and swap targets
            // Actually: if truthy, jump. So: if NOT falsy, jump.
            // Simplest: emit the falsy check but invert the final jump
            // For now, use the general comparison approach
            // NOT falsy = truthy → jmp. We can negate by checking !isFalsy
            // Let's just emit comparison to null/bool/int and jump if NOT any of those
            // This is complex — fall through to default for now
            break; // fallback to default
        }

        case OP_NOT:
            emitCallHelper2(jit.buf, jit.size, (void*)jitNot, A, B, &errorExitPatches);
            break;

        case OP_AND: {
            size_t p = emitJmpFalse(jit.buf, jit.size, A);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_OR: {
            // If truthy (not falsy), skip — emit jmpFalse but invert
            // Load value, check falsy conditions, if NONE match → truthy → jump
            size_t p = emitJmpFalse(jit.buf, jit.size, A);
            // emitJmpFalse jumps when falsy. For OR we want to jump when truthy.
            // So: emit jmp to target (truthy path), patch falsy to fall through
            int target = static_cast<int>(i) + 1 + sBx;
            // Invert: emit unconditional jmp to target, patch falsy jump to here
            emit8(jit.buf, jit.size, 0xe9);
            size_t truthyJmp = jit.size;
            emit32(jit.buf, jit.size, 0);
            patchRel32(jit.buf, p, jit.size);
            patches.push_back({truthyJmp, static_cast<size_t>(target)});
            break;
        }

        case OP_EQ: emitCmp(jit.buf, jit.size, A, B, C, 0x94); break;  // sete
        case OP_NEQ: emitCmp(jit.buf, jit.size, A, B, C, 0x95); break; // setne
        case OP_LT: emitCmp(jit.buf, jit.size, A, B, C, 0x9c); break;  // setl
        case OP_GT: emitCmp(jit.buf, jit.size, A, B, C, 0x9f); break;  // setg
        case OP_LTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9e); break; // setle
        case OP_GTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9d); break; // setge

        case OP_DIV:
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((3 << 16) | (B << 8) | C), &errorExitPatches);
            break;
        case OP_MOD:
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((4 << 16) | (B << 8) | C), &errorExitPatches);
            break;

        case OP_TABLE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitTable, A, B, &errorExitPatches);
            break;

        case OP_MCALL: {
            // Pass: regs(rdi=rbx), state(rsi=r12), base(edx=A), argc(ecx=B), methodStr(r8=pointer)
            BblString* methodStr = chunk.constants[C].stringVal();
            // mov rdi, rbx
            uint8_t a1[] = { 0x48, 0x89, 0xdf }; emit(jit.buf, jit.size, a1, 3);
            // mov rsi, r12
            uint8_t a2[] = { 0x4c, 0x89, 0xe6 }; emit(jit.buf, jit.size, a2, 3);
            // mov edx, A
            uint8_t a3[] = { 0xba }; emit(jit.buf, jit.size, a3, 1); emit32(jit.buf, jit.size, A);
            // mov ecx, B
            uint8_t a4[] = { 0xb9 }; emit(jit.buf, jit.size, a4, 1); emit32(jit.buf, jit.size, B);
            // movabs r8, methodStr
            uint8_t movr8[] = { 0x49, 0xb8 }; emit(jit.buf, jit.size, movr8, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(methodStr));
            // movabs rax, jitMcall
            uint8_t movabs[] = { 0x48, 0xb8 }; emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitMcall));
            // call rax
            uint8_t call[] = { 0xff, 0xd0 }; emit(jit.buf, jit.size, call, 2);
            emitErrorCheck(jit.buf, jit.size, errorExitPatches);
            break;
        }

        case OP_VECTOR:
            emitCallHelper3(jit.buf, jit.size, (void*)jitVector, 0, A, static_cast<uint32_t>((B << 8) | C), &errorExitPatches);
            break;
        case OP_BINARY:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBinary, A, B, &errorExitPatches);
            break;
        case OP_LENGTH:
            emitCallHelper2(jit.buf, jit.size, (void*)jitLength, A, B, &errorExitPatches);
            break;
        case OP_GETFIELD:
            emitCallHelper3(jit.buf, jit.size, (void*)jitGetField, 0, A, static_cast<uint32_t>((B << 8) | C), &errorExitPatches);
            break;
        case OP_SETFIELD:
            emitCallHelper3(jit.buf, jit.size, (void*)jitSetField, 0, A, static_cast<uint32_t>((B << 8) | C), &errorExitPatches);
            break;
        case OP_GETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetIndex, static_cast<uint32_t>((A << 16) | (B << 8) | C), 0, &errorExitPatches);
            break;
        case OP_SETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetIndex, static_cast<uint32_t>((A << 16) | (B << 8) | C), 0, &errorExitPatches);
            break;
        case OP_EXEC:
            emitCallHelper2(jit.buf, jit.size, (void*)jitExec, A, B, &errorExitPatches);
            break;
        case OP_GETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetCapture, A, B, &errorExitPatches);
            break;
        case OP_SETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetCapture, A, B, &errorExitPatches);
            break;
        case OP_TAILCALL:
            emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B, &errorExitPatches);
            break;
        case OP_SIZEOF:
            emitCallHelper3(jit.buf, jit.size, (void*)jitSizeof, 0, A, B, &errorExitPatches);
            break;
        case OP_STRUCT:
            emitCallHelper3(jit.buf, jit.size, (void*)jitStruct, 0, A, static_cast<uint32_t>((B << 8) | C), &errorExitPatches);
            break;
        case OP_EXECFILE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitExecFile, A, B, &errorExitPatches);
            break;
        case OP_BAND:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((0<<16)|(B<<8)|C), &errorExitPatches);
            break;
        case OP_BOR:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((1<<16)|(B<<8)|C), &errorExitPatches);
            break;
        case OP_BXOR:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((2<<16)|(B<<8)|C), &errorExitPatches);
            break;
        case OP_BNOT:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((3<<16)|(B<<8)|0), &errorExitPatches);
            break;
        case OP_SHL:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((4<<16)|(B<<8)|C), &errorExitPatches);
            break;
        case OP_SHR:
            emitCallHelper2(jit.buf, jit.size, (void*)jitBitwise, A, static_cast<uint32_t>((5<<16)|(B<<8)|C), &errorExitPatches);
            break;
        case OP_TRYBEGIN: {
            int catchTarget = static_cast<int>(i) + 1 + sBx;
            tryStack.push_back({catchTarget, std::move(errorExitPatches), A});
            errorExitPatches.clear();
            break;
        }
        case OP_TRYEND: {
            if (!tryStack.empty()) {
                auto info = std::move(tryStack.back());
                tryStack.pop_back();
                for (size_t p : errorExitPatches) {
                    tryCatchPatches.push_back({p, static_cast<size_t>(info.catchTarget), info.destReg});
                }
                errorExitPatches = std::move(info.outerPatches);
            }
            break;
        }
        default:
            emitLoadNull(jit.buf, jit.size, A);
            emitReturn(jit.buf, jit.size, A);
            goto done_compile;
        }
    }

done_compile:
    nativeOffsets[chunk.code.size()] = jit.size;

    // Patch forward jumps
    for (auto& p : patches) {
        if (p.targetInst < nativeOffsets.size() && nativeOffsets[p.targetInst] != 0) {
            patchRel32(jit.buf, p.patchOffset, nativeOffsets[p.targetInst]);
        }
    }

    for (size_t p : selfCallPatches) {
        patchRel32(jit.buf, p, 0);
    }

    // Try/catch: emit catch entry stubs that call jitStoreError, then jump to catch bytecode
    for (auto& tc : tryCatchPatches) {
        size_t catchStub = jit.size;
        emitCallHelper2(jit.buf, jit.size, (void*)jitStoreError, tc.destReg, 0);
        // Jump to the catch block's compiled code
        if (tc.catchInst < nativeOffsets.size() && nativeOffsets[tc.catchInst] != 0) {
            emit8(jit.buf, jit.size, 0xe9);
            size_t jmpPos = jit.size;
            emit32(jit.buf, jit.size, 0);
            patchRel32(jit.buf, jmpPos, nativeOffsets[tc.catchInst]);
        }
        patchRel32(jit.buf, tc.patchOffset, catchStub);
    }

    // Error exit: load NaN-boxed null into rax, epilogue
    size_t errorExit = jit.size;
    {
        uint8_t movabs[] = { 0x48, 0xb8 };
        emit(jit.buf, jit.size, movabs, 2);
        emit64(jit.buf, jit.size, BblValue::TAG_NULL);
        emitEpilogue(jit.buf, jit.size);
    }
    for (size_t p : errorExitPatches) {
        patchRel32(jit.buf, p, errorExit);
    }

    mprotect(jit.buf, jit.capacity, PROT_READ | PROT_EXEC);
    return jit;
}

BblValue jitExecute(BblState& state, Chunk& chunk) {
    JitCode jit = jitCompile(state, chunk);

    typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
    JitFn fn = reinterpret_cast<JitFn>(jit.buf);

    if (state.vm->stack.size() < chunk.numRegs)
        state.vm->stack.resize(chunk.numRegs);

    g_jitError = false;
    BblValue result = fn(state.vm->stack.data(), &state, &chunk);

    jitFree(jit);

    if (g_jitError) {
        g_jitError = false;
        throw BBL::Error{g_jitErrorMsg};
    }

    return result;
}

void jitFree(JitCode& jit) {
    if (jit.buf) {
        munmap(jit.buf, jit.capacity);
        jit.buf = nullptr;
    }
}

// ============ Tracing JIT ============

Trace recordTrace(BblState& state, Chunk& chunk, size_t loopPc, BblValue* regs) {
    Trace trace;
    trace.startChunk = &chunk;
    trace.startPc = loopPc;
    trace.valid = false;

    // The LOOP at loopPc jumps back to (loopPc + 1 - sBx).
    uint32_t loopInst = chunk.code[loopPc];
    int sBx = decodesBx(loopInst);
    size_t bodyStart = loopPc + 1 - sBx;

    struct CallInfo { Chunk* chunk; size_t pc; uint8_t regBase; };
    std::vector<CallInfo> callStack;
    callStack.push_back({&chunk, bodyStart, 0});

    Chunk* curChunk = &chunk;
    size_t pc = bodyStart;
    uint8_t regBase = 0;
    int maxRegs = chunk.numRegs;
    int depth = 0;
    int maxEntries = 200;

    while (trace.entries.size() < static_cast<size_t>(maxEntries)) {
        if (pc >= curChunk->code.size()) { return trace; }
        uint32_t inst = curChunk->code[pc];
        uint8_t op = decodeOP(inst);

        if (op == OP_LOOP && curChunk == &chunk && depth == 0) {
            trace.maxRegs = maxRegs;
            trace.valid = true;
            return trace;
        }

        trace.entries.push_back({inst, curChunk, regBase});
        pc++;

        // Follow calls
        if (op == OP_CALL && depth < 4) {
            uint8_t A = decodeA(inst);
            uint8_t base = A + regBase;
            BblValue callee = regs[base];
            if (callee.type() == BBL::Type::Fn && callee.isClosure()) {
                BblClosure* closure = callee.closureVal();
                callStack.push_back({curChunk, pc, regBase});
                curChunk = &closure->chunk;
                pc = 0;
                regBase = base;
                depth++;
                int needed = regBase + closure->chunk.numRegs;
                if (needed > maxRegs) maxRegs = needed;
                continue;
            }
            return trace; // can't inline non-closure calls
        }

        if (op == OP_RETURN && depth > 0) {
            depth--;
            auto& ci = callStack.back();
            curChunk = ci.chunk;
            pc = ci.pc;
            regBase = ci.regBase;
            callStack.pop_back();
            continue;
        }

        if (op == OP_RETURN && depth == 0) {
            return trace; // unexpected return in trace
        }

        // Handle jumps within trace
        if (op == OP_JMP) {
            int off = decodesBx(inst);
            pc = pc - 1 + 1 + off; // pc was already incremented
        }
        if (op == OP_LTJMP || op == OP_LEJMP || op == OP_GTJMP || op == OP_GEJMP) {
            // Record the branch. The next instruction should be JMP (the else path).
            // If condition was true (skip JMP), we skip pc++ again
            // We just record — the compiler will emit a guard
        }
        if (op == OP_JMPFALSE || op == OP_JMPTRUE) {
            // Record — compiler will emit guard
            int off = decodesBx(inst);
            // For recording, we assume the hot path doesn't jump (condition passed)
            // If it did jump, we'd need to follow — for simplicity, don't follow
        }

        // Unsupported for tracing
        if (op == OP_CLOSURE || op == OP_GETGLOBAL || op == OP_SETGLOBAL ||
            op == OP_MCALL || op == OP_TABLE || op == OP_VECTOR ||
            op == OP_TRYBEGIN || op == OP_TRYEND || op == OP_EXEC || op == OP_EXECFILE) {
            // Abort trace for complex ops
            return trace;
        }
    }
    return trace;
}

JitCode compileTrace(BblState& state, Trace& trace) {
    JitCode jit;
    jit.capacity = trace.entries.size() * 64 + 512;
    jit.buf = static_cast<uint8_t*>(
        mmap(nullptr, jit.capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    jit.size = 0;

    emitPrologue(jit.buf, jit.size);

    size_t loopStart = jit.size;
    std::vector<size_t> sideExitPatches;

    for (size_t i = 0; i < trace.entries.size(); i++) {
        auto& entry = trace.entries[i];
        uint32_t inst = entry.inst;
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst) + entry.regBase;
        uint8_t B = decodeB(inst) + entry.regBase;
        uint8_t C = decodeC(inst) + entry.regBase;
        int sBx = decodesBx(inst);

        switch (op) {
        case OP_ADD: emitAdd(jit.buf, jit.size, A, B, C); break;
        case OP_SUB: emitSub(jit.buf, jit.size, A, B, C); break;
        case OP_MUL: emitMul(jit.buf, jit.size, A, B, C); break;
        case OP_ADDI: emitAddi(jit.buf, jit.size, A, sBx); break;
        case OP_SUBI: emitSubi(jit.buf, jit.size, A, sBx); break;
        case OP_MOVE: emitMove(jit.buf, jit.size, A, B); break;
        case OP_LOADINT: emitLoadInt(jit.buf, jit.size, A, sBx); break;
        case OP_LOADNULL: emitLoadNull(jit.buf, jit.size, A); break;
        case OP_LOADK: emitLoadK(jit.buf, jit.size, A, &entry.chunk->constants[decodeBx(inst)]); break;
        case OP_ADDK: {
            int origA = decodeA(inst) + entry.regBase;
            int origB = decodeB(inst) + entry.regBase;
            int origC = decodeC(inst);
            if (entry.chunk->constants[origC].type() != BBL::Type::Int) {
                jitFree(jit); return JitCode{};
            }
            int64_t cv = entry.chunk->constants[origC].intVal();
            uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
            emit(jit.buf, jit.size, mov1, 3);
            emit32(jit.buf, jit.size, origB * VAL_SIZE);
            emitUnboxIntRax(jit.buf, jit.size);
            uint8_t movabs[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, static_cast<uint64_t>(cv));
            uint8_t addrr[] = { 0x48, 0x01, 0xc8 };
            emit(jit.buf, jit.size, addrr, 3);
            emitReboxIntStore(jit.buf, jit.size, origA * VAL_SIZE);
            break;
        }
        case OP_LTJMP: {
            size_t p = emitLtjmp(jit.buf, jit.size, A, B);
            // If condition true (hot path), skip the next JMP in the trace
            // The jl jumps past the side exit — patch to skip next entry
            sideExitPatches.push_back(p);
            break;
        }
        case OP_LEJMP: {
            size_t p = emitLejmp(jit.buf, jit.size, A, B);
            sideExitPatches.push_back(p);
            break;
        }
        case OP_JMP:
            // In the trace, JMPs are the "else" path after a fused compare.
            // If we get here, the compare guard already passed — skip the JMP.
            // (The JMP would exit the loop, but the guard prevents that.)
            break;
        case OP_JMPFALSE:
            // Guard: if value is falsy, side exit
            // For now, skip — the trace assumes the hot path
            break;

        case OP_CALL:
            // Inlined — the callee's instructions follow in the trace
            // Emit a type guard: check that R[A] is still the expected closure
            // For now, skip the guard (trust the trace)
            break;
        case OP_RETURN:
            // Inlined return: the result register was recorded.
            // The caller's dest was set by the CALL instruction that preceded this.
            // Move the return value to the caller's expected register.
            {
                uint8_t retReg = decodeA(inst) + entry.regBase;
                // Find the CALL that inlined this: look back for OP_CALL
                // The CALL's A field is the dest in the caller.
                // For simplicity, emit MOVE retReg → callerDest
                // We need to know the caller's dest — it's stored in the CALL entry
                // Search backwards for the matching CALL
                for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                    if (decodeOP(trace.entries[j].inst) == OP_CALL &&
                        trace.entries[j].regBase < entry.regBase) {
                        uint8_t callA = decodeA(trace.entries[j].inst) + trace.entries[j].regBase;
                        if (retReg != callA)
                            emitMove(jit.buf, jit.size, callA, retReg);
                        break;
                    }
                }
            }
            break;

        case OP_LT: case OP_GT: case OP_LTE: case OP_GTE:
        case OP_EQ: case OP_NEQ: case OP_NOT:
        case OP_LOADBOOL:
        case OP_GETGLOBAL: case OP_SETGLOBAL:
        case OP_GETCAPTURE: case OP_SETCAPTURE:
            // Fall back to C helper for complex ops in trace
            // For now, abort the trace compilation
            jitFree(jit);
            return JitCode{};

        default:
            break;
        }
    }

    // End of trace: jump back to loop start
    {
        emit8(jit.buf, jit.size, 0xe9);
        int32_t rel = static_cast<int32_t>(loopStart - (jit.size + 4));
        emit32(jit.buf, jit.size, static_cast<uint32_t>(rel));
    }

    // Patch side exits: for now, all guards jump to a common exit
    size_t exitLabel = jit.size;
    // Exit: set rax = 0 (not completed), pop regs, ret
    {
        uint8_t xor_rax[] = { 0x48, 0x31, 0xc0 }; // xor rax, rax
        emit(jit.buf, jit.size, xor_rax, 3);
        emitEpilogue(jit.buf, jit.size);
    }

    // Patch all guard jumps to exit label
    for (size_t p : sideExitPatches) {
        patchRel32(jit.buf, p, exitLabel);
    }

    mprotect(jit.buf, jit.capacity, PROT_READ | PROT_EXEC);
    return jit;
}

TraceResult executeTrace(JitCode& jit, BblValue* regs, BblState* state) {
    if (!jit.buf) return {false, 0};
    typedef int64_t (*TraceFn)(BblValue*, BblState*, void*);
    TraceFn fn = reinterpret_cast<TraceFn>(jit.buf);
    int64_t result = fn(regs, state, nullptr);
    return {result != 0, 0};
}
