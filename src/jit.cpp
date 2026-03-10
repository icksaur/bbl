#include "jit.h"
#include "bbl.h"
#include "vm.h"
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_set>
#include <vector>
#include "compat.h"

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
    void jitWithCleanup(BblValue* regs, BblState* state, uint8_t varReg, uint8_t unused);
    void jitInt(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg);
    void jitStepLimitExceeded(BblValue* regs, BblState* state, uint32_t unused1, uint32_t unused2);
    void jitEnvGet(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg);
    void jitEnvSet(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg);
    int64_t jitLoopTrace(BblValue* regs, BblState* state, Chunk* chunk, uint32_t loopPc);
    void jitTableGet(BblValue* regs, BblState* state, uint8_t base, uint8_t argc);
    void jitTableSet(BblValue* regs, BblState* state, uint8_t base, uint8_t argc);
    void jitWriteBarrier(GcObj* container, BblState* state, uint64_t valBits, uint64_t unused);
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
    if (symId < state->vm->globalsFlat.size()) {
        BblValue& v = state->vm->globalsFlat[symId];
        if (v.bits != 0) { regs[destReg] = v; return; }
    }
    auto it = state->vm->globals.find(symId);
    if (it != state->vm->globals.end()) { regs[destReg] = it->second; return; }
    JIT_ERROR(state, "undefined variable");
}

void jitSetGlobal(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg) {
    state->vm->setGlobal(symId, regs[srcReg]);
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
        state->returnValues.clear();
        state->returnCount = callee.cfnVal()(state);
        if (state->returnCount > 1 && state->returnValues.size() >= (size_t)state->returnCount) {
            for (int i = 0; i < state->returnCount; i++)
                regs[base + i] = state->returnValues[i];
        } else {
            regs[base] = state->hasReturn ? state->returnValue : BblValue::makeNull();
        }
    } else if (callee.type() == BBL::Type::Fn && callee.isClosure()) {
        BblClosure* closure = callee.closureVal();

        if (!closure->jitCache) {
            BblClosure* proto = closure->jitProto ? closure->jitProto : closure;
            if (proto->jitCache) {
                closure->jitCache = proto->jitCache;
            } else {
                JitCode* cached = new JitCode(jitCompile(*state, closure->chunk, closure));
                proto->jitCache = cached;
                closure->jitCache = cached;
            }
        }
        typedef BblValue (*JitFn)(BblValue*, BblState*, Chunk*);
        JitFn fn = reinterpret_cast<JitFn>(closure->jitCache->buf);
        regs[base] = fn(&regs[base], state, &closure->chunk);
        if (g_jitError) return;
    } else if (callee.type() == BBL::Type::Fn) {
        JIT_ERROR(state, "raw BblFn calls not supported in JIT mode");
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
    if (!regs[0].isClosure()) JIT_ERROR(state, "no closure for capture");
    BblClosure* closure = regs[0].closureVal();
    closure->captures[capIdx] = regs[srcReg];
    state->writeBarrier(closure, regs[srcReg]);
}

void jitClosure(BblValue* regs, BblState* state, Chunk* chunk, uint8_t destReg, uint16_t protoIdx) {
    BblClosure* proto = chunk->constants[protoIdx].closureVal();
    BblClosure* closure = state->closureSlab.alloc();
    closure->chunk = proto->chunk;
    closure->arity = proto->arity;
    closure->name = proto->name;
    closure->captureDescs = proto->captureDescs;
    closure->captures.resize(proto->captureDescs.size());
    closure->jitProto = proto;
    closure->env = proto->env;
    closure->gcNext = state->nurseryHead; state->nurseryHead = closure;
    state->allocCount++;

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

static bool resolveTypeName(BblState* state, const std::string& name,
                            CType& ctype, size_t& size, std::string& structName) {
    static const std::unordered_map<std::string, std::pair<CType, size_t>> types = {
        {"bool", {CType::Bool, 1}}, {"int8", {CType::Int8, 1}}, {"uint8", {CType::Uint8, 1}},
        {"int16", {CType::Int16, 2}}, {"uint16", {CType::Uint16, 2}},
        {"int32", {CType::Int32, 4}}, {"uint32", {CType::Uint32, 4}},
        {"int64", {CType::Int64, 8}}, {"uint64", {CType::Uint64, 8}},
        {"float32", {CType::Float32, 4}}, {"float64", {CType::Float64, 8}},
    };
    auto it = types.find(name);
    if (it != types.end()) { ctype = it->second.first; size = it->second.second; structName.clear(); return true; }
    auto sit = state->structDescs.find(name);
    if (sit != state->structDescs.end()) { ctype = CType::Struct; size = sit->second.totalSize; structName = name; return true; }
    return false;
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
            tbl->ensureOrder();
            if (tbl->order) for (auto& k : *tbl->order) keys->set(BblValue::makeInt(i++), k);
            regs[base] = BblValue::makeTable(keys);
        } else if (methodStr == state->m.push) {
            for (int _i=0;_i<argc;_i++) { tbl->set(BblValue::makeInt(tbl->nextIntKey), args[_i]); }
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.pop) {
            bool found = false;
            tbl->ensureOrder();
            if (tbl->order) {
                for (auto it = tbl->order->rbegin(); it != tbl->order->rend(); ++it) {
                    if (it->type() == BBL::Type::Int) {
                        regs[base] = tbl->get(*it).value_or(BblValue::makeNull());
                        tbl->del(*it);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) JIT_ERROR(state, "pop: no integer keys");
        } else if (methodStr == state->m.at) {
            size_t idx = static_cast<size_t>(args[0].intVal());
            tbl->ensureOrder();
            if (!tbl->order || idx >= tbl->order->size()) JIT_ERROR(state, "table index out of range");
            BblValue key = (*tbl->order)[idx];
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
        else if (methodStr == state->m.resize) {
            if (args[0].type() != BBL::Type::Int) JIT_ERROR(state, "vector.resize: argument must be an integer");
            int64_t n = args[0].intVal();
            if (n < 0) JIT_ERROR(state, "vector.resize: size must be non-negative");
            vec->data.resize(static_cast<size_t>(n) * vec->elemSize, 0); regs[base] = BblValue::makeNull();
        }
        else if (methodStr == state->m.reserve) {
            int64_t cap = args[0].intVal();
            if (cap < 0) JIT_ERROR(state, "vector.reserve: capacity must be non-negative");
            vec->data.reserve(static_cast<size_t>(cap) * vec->elemSize); regs[base] = BblValue::makeNull();
        }
        else JIT_ERROR(state, "unknown vector method: " + methodStr->data);
    } else if (receiver.type() == BBL::Type::String) {
        BblString* str = receiver.stringVal();
        switch (methodStr->methodId) {
        case MID_LENGTH: regs[base] = BblValue::makeInt(static_cast<int64_t>(str->data.size())); break;
        case MID_AT: {
            size_t idx = static_cast<size_t>(args[0].intVal());
            if (idx >= str->data.size()) JIT_ERROR(state, "string index out of bounds");
            regs[base] = BblValue::makeString(state->allocString(std::string(1, str->data[idx])));
            break;
        }
        case MID_SLICE: {
            int64_t start = args[0].intVal();
            int64_t len = argc > 1 ? args[1].intVal() : static_cast<int64_t>(str->data.size()) - start;
            uint32_t us = static_cast<uint32_t>(start), ul = static_cast<uint32_t>(len);
            size_t ci = (reinterpret_cast<uintptr_t>(str) ^ (us * 2654435761u) ^ ul) % BblState::SLICE_CACHE_SIZE;
            auto& ce = state->sliceCache[ci];
            if (ce.src == str && ce.pos == us && ce.len == ul) {
                regs[base] = BblValue::makeString(ce.result);
            } else {
                BblString* r = state->intern(str->data.substr(us, ul));
                ce = {str, us, ul, r};
                regs[base] = BblValue::makeString(r);
            }
            break;
        }
        case MID_FIND: {
            size_t start = argc > 1 ? static_cast<size_t>(args[1].intVal()) : 0;
            auto pos = str->data.find(args[0].stringVal()->data, start);
            regs[base] = BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
            break;
        }
        case MID_CONTAINS: regs[base] = BblValue::makeBool(str->data.find(args[0].stringVal()->data) != std::string::npos); break;
        case MID_STARTS_WITH: regs[base] = BblValue::makeBool(str->data.starts_with(args[0].stringVal()->data)); break;
        case MID_ENDS_WITH: regs[base] = BblValue::makeBool(str->data.ends_with(args[0].stringVal()->data)); break;
        case MID_UPPER: { std::string r = str->data; for (auto& c : r) c = static_cast<char>(toupper(c)); regs[base] = BblValue::makeString(state->allocString(std::move(r))); break; }
        case MID_LOWER: { std::string r = str->data; for (auto& c : r) c = static_cast<char>(tolower(c)); regs[base] = BblValue::makeString(state->allocString(std::move(r))); break; }
        case MID_TRIM: {
            auto s = str->data.find_first_not_of(" \t\n\r");
            auto e = str->data.find_last_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(s == std::string::npos ? "" : str->data.substr(s, e - s + 1)));
            break;
        }
        case MID_REPLACE: {
            std::string from = args[0].stringVal()->data;
            if (from.empty()) JIT_ERROR(state, "string.replace: search string must not be empty");
            std::string result = str->data, to = args[1].stringVal()->data;
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) { result.replace(pos, from.size(), to); pos += to.size(); }
            regs[base] = BblValue::makeString(state->intern(result));
            break;
        }
        case MID_JOIN: {
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
            break;
        }
        case MID_SPLIT: {
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
            break;
        }
        case MID_TRIM_LEFT: {
            auto s = str->data.find_first_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(s == std::string::npos ? "" : str->data.substr(s)));
            break;
        }
        case MID_TRIM_RIGHT: {
            auto e = str->data.find_last_not_of(" \t\n\r");
            regs[base] = BblValue::makeString(state->intern(e == std::string::npos ? "" : str->data.substr(0, e + 1)));
            break;
        }
        case MID_PAD_LEFT: {
            int64_t width = args[0].intVal();
            char fill = argc > 1 ? args[1].stringVal()->data[0] : ' ';
            std::string r = str->data;
            if (static_cast<int64_t>(r.size()) < width) r.insert(0, static_cast<size_t>(width) - r.size(), fill);
            regs[base] = BblValue::makeString(state->intern(r));
            break;
        }
        case MID_PAD_RIGHT: {
            int64_t width = args[0].intVal();
            char fill = argc > 1 ? args[1].stringVal()->data[0] : ' ';
            std::string r = str->data;
            if (static_cast<int64_t>(r.size()) < width) r.append(static_cast<size_t>(width) - r.size(), fill);
            regs[base] = BblValue::makeString(state->intern(r));
            break;
        }
        default: JIT_ERROR(state, "unknown string method: " + methodStr->data);
        }
    } else if (receiver.type() == BBL::Type::Binary) {
        BblBinary* bin = receiver.binaryVal();
        if (methodStr == state->m.length) regs[base] = BblValue::makeInt(static_cast<int64_t>(bin->length()));
        else if (methodStr == state->m.at) {
            bin->materialize();
            int64_t idx = args[0].intVal();
            if (idx < 0 || static_cast<size_t>(idx) >= bin->length()) JIT_ERROR(state, "binary index out of bounds");
            regs[base] = BblValue::makeInt(bin->data[static_cast<size_t>(idx)]);
        } else if (methodStr == state->m.set) {
            bin->materialize();
            int64_t idx = args[0].intVal();
            if (idx < 0 || static_cast<size_t>(idx) >= bin->length()) JIT_ERROR(state, "binary index out of bounds");
            if (args[1].type() != BBL::Type::Int) JIT_ERROR(state, "binary.set: value must be integer");
            bin->data[static_cast<size_t>(idx)] = static_cast<uint8_t>(args[1].intVal());
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.slice) {
            bin->materialize();
            int64_t s = args[0].intVal(), l = args[1].intVal();
            if (s < 0 || l < 0 || static_cast<size_t>(s + l) > bin->length()) JIT_ERROR(state, "binary.slice: out of bounds");
            regs[base] = BblValue::makeBinary(state->allocBinary(std::vector<uint8_t>(bin->data.begin() + s, bin->data.begin() + s + l)));
        } else if (methodStr == state->m.resize) {
            bin->materialize();
            int64_t sz = args[0].intVal();
            if (sz < 0) JIT_ERROR(state, "binary.resize: size must be non-negative");
            bin->data.resize(static_cast<size_t>(sz), 0);
            regs[base] = BblValue::makeNull();
        } else if (methodStr == state->m.copy_from) {
            bin->materialize();
            if (args[0].type() != BBL::Type::Binary) JIT_ERROR(state, "binary.copy-from: source must be binary");
            BblBinary* src = args[0].binaryVal();
            src->materialize();
            int64_t dO = argc > 1 ? args[1].intVal() : 0;
            int64_t sO = argc > 2 ? args[2].intVal() : 0;
            int64_t ln = argc > 3 ? args[3].intVal() : static_cast<int64_t>(src->length()) - sO;
            if (dO < 0 || sO < 0 || ln < 0) JIT_ERROR(state, "binary.copy-from: negative offset");
            if (static_cast<size_t>(dO + ln) > bin->length()) JIT_ERROR(state, "binary.copy-from: destination overflow");
            if (static_cast<size_t>(sO + ln) > src->length()) JIT_ERROR(state, "binary.copy-from: source overflow");
            std::memcpy(bin->data.data() + dO, src->data.data() + sO, static_cast<size_t>(ln));
            regs[base] = BblValue::makeNull();
        } else if (methodStr->methodId == MID_AS) {
            bin->materialize();
            if (argc < 2) JIT_ERROR(state, "binary.as: requires type and offset");
            if (args[0].type() != BBL::Type::String) JIT_ERROR(state, "binary.as: type must be a string");
            int64_t off = args[1].intVal();
            if (off < 0) JIT_ERROR(state, "binary.as: negative offset");
            CType ct; size_t sz; std::string sn;
            if (!resolveTypeName(state, args[0].stringVal()->data, ct, sz, sn))
                JIT_ERROR(state, "binary.as: unknown type " + args[0].stringVal()->data);
            if (static_cast<size_t>(off) + sz > bin->data.size()) JIT_ERROR(state, "binary.as: out of bounds");
            const uint8_t* p = bin->data.data() + off;
            if (ct == CType::Struct) {
                auto& desc = state->structDescs[sn];
                BblStruct* s = state->allocStruct(&desc);
                std::memcpy(s->data.data(), p, sz);
                regs[base] = BblValue::makeStruct(s);
            } else {
                FieldDesc fd{"", 0, sz, ct, ""};
                BblStruct tmp; tmp.data.assign(p, p + sz);
                regs[base] = state->readField(&tmp, fd);
            }
        } else if (methodStr->methodId == MID_SET_AS) {
            bin->materialize();
            if (argc < 3) JIT_ERROR(state, "binary.set-as: requires type, offset, and value");
            if (args[0].type() != BBL::Type::String) JIT_ERROR(state, "binary.set-as: type must be a string");
            int64_t off = args[1].intVal();
            if (off < 0) JIT_ERROR(state, "binary.set-as: negative offset");
            CType ct; size_t sz; std::string sn;
            if (!resolveTypeName(state, args[0].stringVal()->data, ct, sz, sn))
                JIT_ERROR(state, "binary.set-as: unknown type " + args[0].stringVal()->data);
            if (static_cast<size_t>(off) + sz > bin->data.size()) JIT_ERROR(state, "binary.set-as: out of bounds");
            uint8_t* p = bin->data.data() + off;
            if (ct == CType::Struct) {
                if (args[2].type() != BBL::Type::Struct) JIT_ERROR(state, "binary.set-as: value must be a struct");
                std::memcpy(p, args[2].structVal()->data.data(), sz);
            } else {
                FieldDesc fd{"", 0, sz, ct, ""};
                BblStruct tmp; tmp.data.resize(sz, 0);
                state->writeField(&tmp, fd, args[2]);
                std::memcpy(p, tmp.data.data(), sz);
            }
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
    regs[destReg] = state->execExpr(regs[srcReg].stringVal()->data);
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
    JIT_ERROR(state, "size-of: unknown type or variable: " + tname);
    JIT_CATCH
}

void jitExecFile(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    JIT_TRY
    if (regs[srcReg].type() != BBL::Type::String) JIT_ERROR(state, "exec-file: argument must be string");
    regs[destReg] = state->execfileExpr(regs[srcReg].stringVal()->data);
    JIT_CATCH
}

void jitStoreError(BblValue* regs, BblState* state, uint8_t destReg, uint8_t unused) {
    (void)unused;
    regs[destReg] = BblValue::makeString(state->intern(g_jitErrorMsg));
    g_jitError = false;
    g_jitErrorMsg.clear();
}

void jitWithCleanup(BblValue* regs, BblState* state, uint8_t varReg, uint8_t unused) {
    (void)unused; (void)state;
    BblValue& val = regs[varReg];
    if (val.type() == BBL::Type::UserData && val.userdataVal()->desc &&
        val.userdataVal()->desc->destructor && val.userdataVal()->data) {
        val.userdataVal()->desc->destructor(val.userdataVal()->data);
        val.userdataVal()->data = nullptr;
    }
    val = BblValue::makeNull();
}

void jitInt(BblValue* regs, BblState* state, uint8_t destReg, uint8_t srcReg) {
    (void)state;
    BblValue& arg = regs[srcReg];
    if (arg.type() == BBL::Type::Int) { regs[destReg] = arg; return; }
    if (arg.type() == BBL::Type::Float) {
        regs[destReg] = BblValue::makeInt(static_cast<int64_t>(arg.floatVal())); return;
    }
    if (arg.type() == BBL::Type::String) {
        const char* s = arg.stringVal()->data.c_str();
        char* end = nullptr;
        int64_t val = strtoll(s, &end, 10);
        if (end != s && *end == '\0') { regs[destReg] = BblValue::makeInt(val); return; }
    }
    JIT_ERROR(state, "int: cannot convert");
}

void jitStepLimitExceeded(BblValue*, BblState* state, uint32_t, uint32_t) {
    JIT_ERROR(state, "step limit exceeded: " + std::to_string(state->maxSteps) + " steps");
}

void jitEnvGet(BblValue* regs, BblState* state, uint32_t symId, uint8_t destReg) {
    BblTable* env = nullptr;
    if (regs[0].type() == BBL::Type::Fn && regs[0].isClosure())
        env = regs[0].closureVal()->env;
    if (!env) env = state->currentEnv;
    if (env) {
        auto nit = state->symbolNames.find(symId);
        if (nit == state->symbolNames.end()) {
            for (auto& [name, id] : state->symbolIds) {
                if (id == symId) { nit = state->symbolNames.emplace(symId, state->intern(name)).first; break; }
            }
        }
        if (nit != state->symbolNames.end()) {
            auto v = env->get(BblValue::makeString(nit->second));
            if (v) { regs[destReg] = *v; return; }
        }
    }
    BblValue* gv = state->vm->getGlobal(symId);
    if (gv) { regs[destReg] = *gv; return; }
    JIT_ERROR(state, "undefined variable");
}

void jitEnvSet(BblValue* regs, BblState* state, uint32_t symId, uint8_t srcReg) {
    BblTable* env = nullptr;
    if (regs[0].type() == BBL::Type::Fn && regs[0].isClosure())
        env = regs[0].closureVal()->env;
    if (!env) env = state->currentEnv;
    if (env) {
        auto nit = state->symbolNames.find(symId);
        if (nit == state->symbolNames.end()) {
            for (auto& [name, id] : state->symbolIds) {
                if (id == symId) { nit = state->symbolNames.emplace(symId, state->intern(name)).first; break; }
            }
        }
        if (nit != state->symbolNames.end()) {
            env->set(BblValue::makeString(nit->second), regs[srcReg]);
            return;
        }
    }
    state->vm->setGlobal(symId, regs[srcReg]);
}

int64_t jitLoopTrace(BblValue* regs, BblState* state, Chunk* chunk, uint32_t loopPc) {
    auto& lt = chunk->loopTraces[loopPc];
    if (lt.blacklisted) return -1;
    if (state->maxSteps > 0) return -1;

    if (!lt.compiled) {
        lt.hotCount++;
        if (lt.hotCount < 32) return -1;
        lt.hotCount = 0;

        Trace trace = recordTrace(*state, *chunk, loopPc, regs);
        if (!trace.valid) { lt.blacklisted = true; return -1; }

        optimizeTrace(*state, trace);
        JitCode jit = compileTrace(*state, trace);
        if (!jit.buf) { lt.blacklisted = true; return -1; }

        lt.code = jit.buf;
        lt.capacity = jit.capacity;
        lt.compiled = true;
        lt.snapshots = new std::vector<Snapshot>(std::move(trace.snapshots));
        if (!trace.sunkAllocs.empty())
            lt.sunkAllocs = new std::vector<SunkAllocation>(std::move(trace.sunkAllocs));
    }

    JitCode traceJit;
    traceJit.buf = static_cast<uint8_t*>(lt.code);
    traceJit.capacity = lt.capacity;

    TraceResult result = executeTrace(traceJit, regs, state);

    if (!result.completed && lt.sunkAllocs) {
        for (auto& sunk : *lt.sunkAllocs) {
            BblTable* tbl = state->allocTable();
            for (auto& f : sunk.fields)
                tbl->set(BblValue::makeString(state->intern(f.name)), regs[f.srcReg]);
            regs[sunk.destReg] = BblValue::makeTable(tbl);
        }
    }
    return 1;
}

void jitTableGet(BblValue* regs, BblState* state, uint8_t base, uint8_t argc) {
    if (regs[base].type() != BBL::Type::Table) {
        jitMcall(regs, state, base, argc, state->m.get);
        return;
    }
    BblTable* tbl = regs[base].tableVal();
    regs[base] = tbl->get(regs[base + 1]).value_or(
        argc > 1 ? regs[base + 2] : BblValue::makeNull());
}

void jitTableSet(BblValue* regs, BblState* state, uint8_t base, uint8_t argc) {
    if (regs[base].type() != BBL::Type::Table) {
        jitMcall(regs, state, base, argc, state->m.set);
        return;
    }
    regs[base].tableVal()->set(regs[base + 1], regs[base + 2]);
    regs[base] = BblValue::makeNull();
}

void jitWriteBarrier(GcObj* container, BblState* state, uint64_t valBits, uint64_t) {
    BblValue val; val.bits = valBits;
    state->writeBarrier(container, val);
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
            if (A == B && !rb.stringVal()->interned) {
                rb.stringVal()->data += jitValToStr(*state, rc);
            } else {
                regs[A] = BblValue::makeString(state->allocString(rb.stringVal()->data + jitValToStr(*state, rc)));
            }
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

static void emit16(uint8_t* buf, size_t& pos, uint16_t v) {
    memcpy(buf + pos, &v, 2); pos += 2;
}

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
    // test r14d, r14d
    uint8_t test[] = { 0x45, 0x85, 0xf6 };
    emit(buf, pos, test, 3);
    // jnz lightRet (+12: add rsp,8(4) + pop r14(2) + pop r13(2) + pop r12(2) + pop rbx(1) + ret(1))
    uint8_t jnz[] = { 0x75, 0x0c };
    emit(buf, pos, jnz, 2);
    // Full epilogue: add rsp,8; pop r14, r13, r12, rbx, ret
    uint8_t fullEpi[] = { 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5b, 0xc3 };
    emit(buf, pos, fullEpi, sizeof(fullEpi));
    // Light epilogue: add rsp,8; pop r14, pop rbx, ret
    uint8_t lightEpi[] = { 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5e, 0x5b, 0xc3 };
    emit(buf, pos, lightEpi, sizeof(lightEpi));
}

// ADD R[A] = R[B] + R[C] (integer, NaN-boxed — direct arithmetic, no unbox)
static void emitAdd(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    uint8_t add[] = { 0x48, 0x03, 0x83 };
    emit(buf, pos, add, 3);
    emit32(buf, pos, C * VAL_SIZE);
    // a+b has 2×TAG. Subtract one TAG to get TAG|(va+vb).
    uint8_t movabs[] = { 0x48, 0xB9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    uint8_t sub[] = { 0x48, 0x29, 0xC8 };
    emit(buf, pos, sub, 3);
    uint8_t store[] = { 0x48, 0x89, 0x83 };
    emit(buf, pos, store, 3);
    emit32(buf, pos, A * VAL_SIZE);
}

static void emitSub(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t load[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, load, 3);
    emit32(buf, pos, B * VAL_SIZE);
    uint8_t sub_op[] = { 0x48, 0x2b, 0x83 };
    emit(buf, pos, sub_op, 3);
    emit32(buf, pos, C * VAL_SIZE);
    // a-b cancels TAG. Add it back.
    uint8_t movabs[] = { 0x48, 0xB9 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, NB_TAG_INT);
    uint8_t add[] = { 0x48, 0x01, 0xC8 };
    emit(buf, pos, add, 3);
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
    // sub qword [rbx + A*8], imm32 (works on NaN-boxed: decrements lower bits)
    uint8_t subMem[] = { 0x48, 0x81, 0xab };
    emit(buf, pos, subMem, 3);
    emit32(buf, pos, A * VAL_SIZE);
    emit32(buf, pos, static_cast<uint32_t>(imm));
}

static void emitFAdd(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t ld[] = { 0xf2, 0x0f, 0x10, 0x83 }; emit(buf, pos, ld, 4); emit32(buf, pos, B * VAL_SIZE);
    uint8_t op[] = { 0xf2, 0x0f, 0x58, 0x83 }; emit(buf, pos, op, 4); emit32(buf, pos, C * VAL_SIZE);
    uint8_t st[] = { 0xf2, 0x0f, 0x11, 0x83 }; emit(buf, pos, st, 4); emit32(buf, pos, A * VAL_SIZE);
}
static void emitFSub(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t ld[] = { 0xf2, 0x0f, 0x10, 0x83 }; emit(buf, pos, ld, 4); emit32(buf, pos, B * VAL_SIZE);
    uint8_t op[] = { 0xf2, 0x0f, 0x5c, 0x83 }; emit(buf, pos, op, 4); emit32(buf, pos, C * VAL_SIZE);
    uint8_t st[] = { 0xf2, 0x0f, 0x11, 0x83 }; emit(buf, pos, st, 4); emit32(buf, pos, A * VAL_SIZE);
}
static void emitFMul(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t ld[] = { 0xf2, 0x0f, 0x10, 0x83 }; emit(buf, pos, ld, 4); emit32(buf, pos, B * VAL_SIZE);
    uint8_t op[] = { 0xf2, 0x0f, 0x59, 0x83 }; emit(buf, pos, op, 4); emit32(buf, pos, C * VAL_SIZE);
    uint8_t st[] = { 0xf2, 0x0f, 0x11, 0x83 }; emit(buf, pos, st, 4); emit32(buf, pos, A * VAL_SIZE);
}
static void emitFDiv(uint8_t* buf, size_t& pos, int A, int B, int C) {
    uint8_t ld[] = { 0xf2, 0x0f, 0x10, 0x83 }; emit(buf, pos, ld, 4); emit32(buf, pos, B * VAL_SIZE);
    uint8_t op[] = { 0xf2, 0x0f, 0x5e, 0x83 }; emit(buf, pos, op, 4); emit32(buf, pos, C * VAL_SIZE);
    uint8_t st[] = { 0xf2, 0x0f, 0x11, 0x83 }; emit(buf, pos, st, 4); emit32(buf, pos, A * VAL_SIZE);
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

static size_t emitCompareJmpDirect(uint8_t* buf, size_t& pos, int A, int B, uint8_t jccByte) {
    // Both operands known TAG_INT — unbox and compare payloads (signed)
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, A * VAL_SIZE); // mov rax, [rbx+A*8]
    uint8_t shl1[] = { 0x48, 0xc1, 0xe0, 0x10 };
    emit(buf, pos, shl1, 4); // shl rax, 16
    uint8_t sar1[] = { 0x48, 0xc1, 0xf8, 0x10 };
    emit(buf, pos, sar1, 4); // sar rax, 16 (sign-extend payload)
    uint8_t mov2[] = { 0x48, 0x8b, 0x8b };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, B * VAL_SIZE); // mov rcx, [rbx+B*8]
    uint8_t shl2[] = { 0x48, 0xc1, 0xe1, 0x10 };
    emit(buf, pos, shl2, 4); // shl rcx, 16
    uint8_t sar2[] = { 0x48, 0xc1, 0xf9, 0x10 };
    emit(buf, pos, sar2, 4); // sar rcx, 16
    uint8_t cmp[] = { 0x48, 0x39, 0xc8 };
    emit(buf, pos, cmp, 3); // cmp rax, rcx
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

static void emitLineStore(uint8_t* buf, size_t& pos, size_t runtimeLineOff, int line) {
    // mov dword [r12 + offset], imm32
    uint8_t prefix[] = { 0x41, 0xc7, 0x84, 0x24 };
    emit(buf, pos, prefix, 4);
    emit32(buf, pos, static_cast<uint32_t>(runtimeLineOff));
    emit32(buf, pos, static_cast<uint32_t>(line));
}

static void emitWriteBarrier(uint8_t* buf, size_t& pos) {
    // rdi = container (GcObj*), rax = stored value
    // Fast path: test byte [rdi+2], 1  (check container->old)
    uint8_t testOld[] = { 0xf6, 0x47, 0x02, 0x01 };
    emit(buf, pos, testOld, 4);
    // jz skip (container is young → no barrier)
    uint8_t jz[] = { 0x74 };
    emit(buf, pos, jz, 1);
    size_t skipPatch = pos;
    emit8(buf, pos, 0);
    // Slow path: call jitWriteBarrier(container=rdi, state=r12, valBits=rax, 0)
    // rdi already = container, rsi = r12 (state)
    uint8_t movRsi[] = { 0x4c, 0x89, 0xe6 }; // mov rsi, r12
    emit(buf, pos, movRsi, 3);
    uint8_t movRdx[] = { 0x48, 0x89, 0xc2 }; // mov rdx, rax
    emit(buf, pos, movRdx, 3);
    uint8_t xorRcx[] = { 0x31, 0xc9 }; // xor ecx, ecx
    emit(buf, pos, xorRcx, 2);
    // Save rax (value) across call
    uint8_t pushRax[] = { 0x50 };
    emit(buf, pos, pushRax, 1);
    uint8_t pushRdi[] = { 0x57 };
    emit(buf, pos, pushRdi, 1);
    uint8_t movabs[] = { 0x48, 0xb8 };
    emit(buf, pos, movabs, 2);
    emit64(buf, pos, reinterpret_cast<uint64_t>((void*)jitWriteBarrier));
    uint8_t call[] = { 0xff, 0xd0 };
    emit(buf, pos, call, 2);
    uint8_t popRdi[] = { 0x5f };
    emit(buf, pos, popRdi, 1);
    uint8_t popRax[] = { 0x58 };
    emit(buf, pos, popRax, 1);
    // skip:
    buf[skipPatch] = static_cast<uint8_t>(pos - skipPatch - 1);
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

static void emitStepCheck(uint8_t* buf, size_t& pos, size_t stepCountOff, size_t maxStepsOff, std::vector<size_t>& errorExitPatches) {
    // inc qword [r12 + stepCountOff]
    emit(buf, pos, (uint8_t[]){ 0x49, 0xff, 0x84, 0x24 }, 4);
    emit32(buf, pos, static_cast<uint32_t>(stepCountOff));
    // mov rax, [r12 + stepCountOff]
    emit(buf, pos, (uint8_t[]){ 0x49, 0x8b, 0x84, 0x24 }, 4);
    emit32(buf, pos, static_cast<uint32_t>(stepCountOff));
    // cmp rax, [r12 + maxStepsOff]
    emit(buf, pos, (uint8_t[]){ 0x49, 0x3b, 0x84, 0x24 }, 4);
    emit32(buf, pos, static_cast<uint32_t>(maxStepsOff));
    // jbe skip
    emit(buf, pos, (uint8_t[]){ 0x76 }, 1);
    size_t skipPatch = pos;
    emit8(buf, pos, 0);
    emitCallHelper2(buf, pos, (void*)jitStepLimitExceeded, 0, 0, &errorExitPatches);
    buf[skipPatch] = static_cast<uint8_t>(pos - skipPatch - 1);
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

static void emitCmpDirect(uint8_t* buf, size_t& pos, int A, int B, int C, uint8_t jccOpcode) {
    // Both known TAG_INT — unbox and compare payloads (signed)
    uint8_t mov1[] = { 0x48, 0x8b, 0x83 };
    emit(buf, pos, mov1, 3);
    emit32(buf, pos, B * VAL_SIZE); // mov rax, [rbx+B*8]
    uint8_t shl1[] = { 0x48, 0xc1, 0xe0, 0x10 };
    emit(buf, pos, shl1, 4);
    uint8_t sar1[] = { 0x48, 0xc1, 0xf8, 0x10 };
    emit(buf, pos, sar1, 4); // sign-extend A
    uint8_t mov2[] = { 0x48, 0x8b, 0x8b };
    emit(buf, pos, mov2, 3);
    emit32(buf, pos, C * VAL_SIZE);
    uint8_t shl2[] = { 0x48, 0xc1, 0xe1, 0x10 };
    emit(buf, pos, shl2, 4);
    uint8_t sar2[] = { 0x48, 0xc1, 0xf9, 0x10 };
    emit(buf, pos, sar2, 4); // sign-extend B
    uint8_t cmp[] = { 0x48, 0x39, 0xc8 };
    emit(buf, pos, cmp, 3); // cmp rax, rcx
    uint8_t setcc[] = { 0x0f, jccOpcode, 0xc0 };
    emit(buf, pos, setcc, 3);
    uint8_t movzx[] = { 0x0f, 0xb6, 0xc0 };
    emit(buf, pos, movzx, 3);
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
    size_t patchOffset;
    size_t targetInst;
};

struct HwReg { uint8_t code; bool extended; };
static constexpr HwReg hwRegs[] = {
    {0, true},  {1, true},  {2, true},  {3, true},  // r8-r11
    {2, false}, {6, false}, {7, false},              // rdx, rsi, rdi
};
static constexpr int MAX_HW_REGS = 7;

static uint8_t rexRR(int regField, int rmField) {
    uint8_t rex = 0x48;
    if (hwRegs[regField].extended) rex |= 0x04;
    if (hwRegs[rmField].extended) rex |= 0x01;
    return rex;
}
static uint8_t modrmRR(int reg, int rm) {
    return 0xC0 | (hwRegs[reg].code << 3) | hwRegs[rm].code;
}
static void emitMovRR(uint8_t* buf, size_t& pos, int dst, int src) {
    if (dst == src) return;
    emit8(buf, pos, rexRR(src, dst)); emit8(buf, pos, 0x89); emit8(buf, pos, modrmRR(src, dst));
}
static void emitAddRR(uint8_t* buf, size_t& pos, int dst, int src) {
    emit8(buf, pos, rexRR(src, dst)); emit8(buf, pos, 0x01); emit8(buf, pos, modrmRR(src, dst));
}
static void emitSubRR(uint8_t* buf, size_t& pos, int dst, int src) {
    emit8(buf, pos, rexRR(src, dst)); emit8(buf, pos, 0x29); emit8(buf, pos, modrmRR(src, dst));
}
static void emitCmpRR(uint8_t* buf, size_t& pos, int a, int b) {
    emit8(buf, pos, rexRR(b, a)); emit8(buf, pos, 0x39); emit8(buf, pos, modrmRR(b, a));
}
static void emitImulRR(uint8_t* buf, size_t& pos, int dst, int src) {
    emit8(buf, pos, rexRR(dst, src)); emit8(buf, pos, 0x0F); emit8(buf, pos, 0xAF); emit8(buf, pos, modrmRR(dst, src));
}
static void emitAddImmR(uint8_t* buf, size_t& pos, int dst, int32_t imm) {
    uint8_t rex = 0x48; if (hwRegs[dst].extended) rex |= 0x01;
    if (imm >= -128 && imm <= 127) {
        emit8(buf, pos, rex); emit8(buf, pos, 0x83);
        emit8(buf, pos, 0xC0 | hwRegs[dst].code); emit8(buf, pos, static_cast<uint8_t>(imm));
    } else {
        emit8(buf, pos, rex); emit8(buf, pos, 0x81);
        emit8(buf, pos, 0xC0 | hwRegs[dst].code); emit32(buf, pos, static_cast<uint32_t>(imm));
    }
}
static void emitSubImmR(uint8_t* buf, size_t& pos, int dst, int32_t imm) {
    uint8_t rex = 0x48; if (hwRegs[dst].extended) rex |= 0x01;
    if (imm >= -128 && imm <= 127) {
        emit8(buf, pos, rex); emit8(buf, pos, 0x83);
        emit8(buf, pos, 0xE8 | hwRegs[dst].code); emit8(buf, pos, static_cast<uint8_t>(imm));
    } else {
        emit8(buf, pos, rex); emit8(buf, pos, 0x81);
        emit8(buf, pos, 0xE8 | hwRegs[dst].code); emit32(buf, pos, static_cast<uint32_t>(imm));
    }
}
static void emitMovImmR(uint8_t* buf, size_t& pos, int dst, int64_t imm) {
    uint8_t rex = 0x48; if (hwRegs[dst].extended) rex |= 0x01;
    emit8(buf, pos, rex); emit8(buf, pos, 0xB8 | hwRegs[dst].code);
    emit64(buf, pos, static_cast<uint64_t>(imm));
}
static void emitLoadFromMemR(uint8_t* buf, size_t& pos, int hwSlot, uint32_t memOff) {
    uint8_t rex = 0x48; if (hwRegs[hwSlot].extended) rex |= 0x04;
    emit8(buf, pos, rex); emit8(buf, pos, 0x8B);
    emit8(buf, pos, 0x83 | (hwRegs[hwSlot].code << 3)); emit32(buf, pos, memOff);
}
static void emitStoreToMemR(uint8_t* buf, size_t& pos, int hwSlot, uint32_t memOff) {
    uint8_t rex = 0x48; if (hwRegs[hwSlot].extended) rex |= 0x04;
    emit8(buf, pos, rex); emit8(buf, pos, 0x89);
    emit8(buf, pos, 0x83 | (hwRegs[hwSlot].code << 3)); emit32(buf, pos, memOff);
}
static void emitUnboxR(uint8_t* buf, size_t& pos, int hwSlot) {
    uint8_t rex = 0x48; if (hwRegs[hwSlot].extended) rex |= 0x01;
    uint8_t rm = hwRegs[hwSlot].code;
    emit8(buf, pos, rex); emit8(buf, pos, 0xC1); emit8(buf, pos, 0xE0 | rm); emit8(buf, pos, 0x10);
    emit8(buf, pos, rex); emit8(buf, pos, 0xC1); emit8(buf, pos, 0xF8 | rm); emit8(buf, pos, 0x10);
}
static void emitReboxStoreR(uint8_t* buf, size_t& pos, int hwSlot, uint32_t memOff) {
    // mov rax, hw_reg
    uint8_t rex = 0x48; if (hwRegs[hwSlot].extended) rex |= 0x04;
    emit8(buf, pos, rex); emit8(buf, pos, 0x89);
    emit8(buf, pos, 0xC0 | (hwRegs[hwSlot].code << 3));
    // and rax, PAYLOAD_MASK
    uint8_t movabs[] = { 0x48, 0xb9 }; emit(buf, pos, movabs, 2);
    emit64(buf, pos, 0x0000FFFFFFFFFFFFULL);
    uint8_t andrr[] = { 0x48, 0x21, 0xc8 }; emit(buf, pos, andrr, 3);
    // or rax, TAG_INT
    emit(buf, pos, movabs, 2); emit64(buf, pos, NB_TAG_INT);
    uint8_t orrr[] = { 0x48, 0x09, 0xc8 }; emit(buf, pos, orrr, 3);
    // mov [rbx + memOff], rax
    uint8_t store[] = { 0x48, 0x89, 0x83 }; emit(buf, pos, store, 3);
    emit32(buf, pos, memOff);
}

struct LoopAlloc {
    size_t startIdx, endIdx, exitIdx;
    size_t nativeBodyStart;
    int8_t regMap[256];
    bool active;
};

JitCode jitCompile(BblState& state, Chunk& chunk, BblClosure* self) {
    JitCode jit;
    jit.capacity = chunk.code.size() * 96 + 1024;
    jit.buf = static_cast<uint8_t*>(
        jitAlloc(jit.capacity));
    jit.size = 0;

    // Full prologue: save callee-saved regs, set up rbx/r12/r13/r14
    uint8_t fullPrologue[] = {
        0x53,                   // push rbx
        0x41, 0x54,             // push r12
        0x41, 0x55,             // push r13
        0x41, 0x56,             // push r14
        0x48, 0x83, 0xec, 0x08, // sub rsp, 8 (alignment)
        0x48, 0x89, 0xfb,       // mov rbx, rdi  (regs)
        0x49, 0x89, 0xf4,       // mov r12, rsi  (state)
        0x49, 0x89, 0xd5,       // mov r13, rdx  (chunk)
        0x41, 0xbe, 0x00, 0x00, 0x00, 0x00, // mov r14d, 0 (full marker)
    };
    emit(jit.buf, jit.size, fullPrologue, sizeof(fullPrologue));
    // Jump over light entry
    emit8(jit.buf, jit.size, 0xeb); // jmp rel8
    size_t jmpOverLight = jit.size;
    emit8(jit.buf, jit.size, 0x00);

    // Light entry: save rbx, set new regs base, r14=1
    size_t lightEntry = jit.size;
    uint8_t lightPrologue[] = {
        0x53,                   // push rbx
        0x41, 0x56,             // push r14
        0x48, 0x83, 0xec, 0x08, // sub rsp, 8 (alignment)
        0x48, 0x89, 0xfb,       // mov rbx, rdi  (regs)
        0x41, 0xbe, 0x01, 0x00, 0x00, 0x00, // mov r14d, 1 (light marker)
    };
    emit(jit.buf, jit.size, lightPrologue, sizeof(lightPrologue));

    jit.buf[jmpOverLight] = static_cast<uint8_t>(jit.size - jmpOverLight - 1);


    std::vector<size_t> nativeOffsets(chunk.code.size() + 1, 0);
    std::vector<JumpPatch> patches;
    std::unordered_map<uint8_t, BblClosure*> closureRegs;
    std::set<uint8_t> selfRefRegs;
    bool hasSelfCalls = false;
    enum class KnownType : uint8_t { Unknown = 0, Int, Table, Float };
    KnownType knownTypes[256];
    memset(knownTypes, 0, sizeof(knownTypes));
    std::vector<size_t> selfCallPatches;
    std::vector<size_t> errorExitPatches;

    struct TryInfo { int catchTarget; std::vector<size_t> outerPatches; uint8_t destReg; };
    std::vector<TryInfo> tryStack;
    struct TryCatchPatch { size_t patchOffset; size_t catchInst; uint8_t destReg; };
    std::vector<TryCatchPatch> tryCatchPatches;

    BblState dummy;
    size_t runtimeLineOff = reinterpret_cast<char*>(&dummy.runtimeLine) - reinterpret_cast<char*>(&dummy);
    size_t stepCountOff = reinterpret_cast<char*>(&dummy.stepCount) - reinterpret_cast<char*>(&dummy);
    size_t maxStepsOff = reinterpret_cast<char*>(&dummy.maxSteps) - reinterpret_cast<char*>(&dummy);
    size_t vmOff = reinterpret_cast<char*>(&dummy.vm) - reinterpret_cast<char*>(&dummy);
    VmState vmDummy;
    size_t globalsFlatOff = reinterpret_cast<char*>(&vmDummy.globalsFlat) - reinterpret_cast<char*>(&vmDummy);
    BblClosure closureDummy;
    size_t capturesOff = reinterpret_cast<char*>(&closureDummy.captures) - reinterpret_cast<char*>(&closureDummy);
    bool emitStepChecks = (state.maxSteps > 0);

    static_assert(sizeof(BblTable::Entry) == 16, "Entry must be 16 bytes for inline cache");
    BblTable tblDummy;
    size_t tblBucketsOff = reinterpret_cast<char*>(&tblDummy.buckets) - reinterpret_cast<char*>(&tblDummy);
    size_t tblCapacityOff = reinterpret_cast<char*>(&tblDummy.capacity) - reinterpret_cast<char*>(&tblDummy);
    size_t tblArrayOff = reinterpret_cast<char*>(&tblDummy.array) - reinterpret_cast<char*>(&tblDummy);
    size_t tblAsizeOff = reinterpret_cast<char*>(&tblDummy.asize) - reinterpret_cast<char*>(&tblDummy);
    size_t tblCountOff = reinterpret_cast<char*>(&tblDummy.count) - reinterpret_cast<char*>(&tblDummy);
    size_t gcTypeOff = reinterpret_cast<char*>(&tblDummy.gcType) - reinterpret_cast<char*>(&tblDummy);
    uint8_t tableGcType = static_cast<uint8_t>(GcType::Table);

    BblVec vecDummy;
    vecDummy.elemSize = 8;
    vecDummy.data.resize(8);
    size_t vecElemSizeOff = reinterpret_cast<char*>(&vecDummy.elemSize) - reinterpret_cast<char*>(&vecDummy);
    size_t vecDataOff = reinterpret_cast<char*>(&vecDummy.data) - reinterpret_cast<char*>(&vecDummy);
    uint8_t* vecDataPtr = vecDummy.data.data();
    size_t vecDataPtrOff = 0;
    for (size_t off = 0; off < sizeof(std::vector<uint8_t>); off += 8) {
        if (*reinterpret_cast<uint8_t**>(reinterpret_cast<char*>(&vecDummy.data) + off) == vecDataPtr) {
            vecDataPtrOff = off; break;
        }
    }
    size_t vecDataSizeOff = 0;
    for (size_t off = 0; off < sizeof(std::vector<uint8_t>); off += 8) {
        if (off == vecDataPtrOff) continue;
        uintptr_t val = *reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(&vecDummy.data) + off);
        if (val == reinterpret_cast<uintptr_t>(vecDataPtr) + 8) {
            vecDataSizeOff = off; break;
        }
    }
    vecDataPtrOff += vecDataOff;
    vecDataSizeOff += vecDataOff;
    uint8_t vecGcType = static_cast<uint8_t>(GcType::Vec);
    size_t vecElemTypeTagOff = reinterpret_cast<char*>(&vecDummy.elemTypeTag) - reinterpret_cast<char*>(&vecDummy);
    uint8_t vecIntTypeTag = static_cast<uint8_t>(BBL::Type::Int);

    BblString strDummy("test");
    size_t strDataOff = reinterpret_cast<char*>(&strDummy.data) - reinterpret_cast<char*>(&strDummy);
    size_t strSizeOff = 0;
    for (size_t off = 0; off < sizeof(std::string); off += 8) {
        if (*reinterpret_cast<size_t*>(reinterpret_cast<char*>(&strDummy.data) + off) == 4) {
            strSizeOff = strDataOff + off; break;
        }
    }

    // Pre-scan: identify eligible arithmetic loops for register allocation
    std::unordered_map<size_t, LoopAlloc> loopAllocMap;
    for (size_t li = 0; li < chunk.code.size(); li++) {
        if (decodeOP(chunk.code[li]) != OP_LOOP) continue;
        int loopsBx = decodesBx(chunk.code[li]);
        int loopStart = static_cast<int>(li) + 1 - loopsBx;
        if (loopStart < 0 || loopStart >= static_cast<int>(li)) continue;

        LoopAlloc la;
        la.startIdx = loopStart;
        la.endIdx = li;
        la.exitIdx = loopStart + 1;
        la.active = true;
        memset(la.regMap, -1, sizeof(la.regMap));

        std::set<uint8_t> usedRegs;
        for (size_t bi = loopStart; bi <= li; bi++) {
            uint8_t bop = decodeOP(chunk.code[bi]);
            if (bop != OP_LOADINT && bop != OP_ADD && bop != OP_SUB && bop != OP_MUL &&
                bop != OP_ADDI && bop != OP_SUBI && bop != OP_ADDK && bop != OP_MOVE &&
                bop != OP_LTJMP && bop != OP_LEJMP && bop != OP_GTJMP && bop != OP_GEJMP &&
                bop != OP_JMP && bop != OP_LOOP &&
                bop != OP_EQ && bop != OP_NEQ && bop != OP_LT && bop != OP_GT &&
                bop != OP_LTE && bop != OP_GTE) {
                la.active = false; break;
            }
            uint8_t bA = decodeA(chunk.code[bi]), bB = decodeB(chunk.code[bi]), bC = decodeC(chunk.code[bi]);
            if (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL) { usedRegs.insert(bA); usedRegs.insert(bB); usedRegs.insert(bC); }
            else if (bop == OP_ADDI || bop == OP_SUBI || bop == OP_LOADINT) { usedRegs.insert(bA); }
            else if (bop == OP_ADDK) { usedRegs.insert(bA); usedRegs.insert(bB); }
            else if (bop == OP_MOVE) { usedRegs.insert(bA); usedRegs.insert(bB); }
            else if (bop == OP_LTJMP || bop == OP_LEJMP || bop == OP_GTJMP || bop == OP_GEJMP) { usedRegs.insert(bA); usedRegs.insert(bB); }
            else if (bop == OP_EQ || bop == OP_NEQ || bop == OP_LT || bop == OP_GT || bop == OP_LTE || bop == OP_GTE) { usedRegs.insert(bA); usedRegs.insert(bB); usedRegs.insert(bC); }
        }
        if (la.active && usedRegs.size() <= MAX_HW_REGS) {
            int slot = 0;
            for (uint8_t reg : usedRegs) la.regMap[reg] = slot++;
            loopAllocMap[loopStart] = la;
        }
    }
    const LoopAlloc* currentLoop = nullptr;

    for (size_t i = 0; i < chunk.code.size(); i++) {
        nativeOffsets[i] = jit.size;
        uint32_t inst = chunk.code[i];
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst), B = decodeB(inst), C = decodeC(inst);
        uint16_t Bx = decodeBx(inst);
        int sBx = decodesBx(inst);
        int lineNum = (i < chunk.lines.size()) ? chunk.lines[i] : 0;
        static thread_local int lastEmittedLine = -1;
        if (lineNum != lastEmittedLine && !currentLoop) {
            emitLineStore(jit.buf, jit.size, runtimeLineOff, lineNum);
            lastEmittedLine = lineNum;
        }

        if (op != OP_GETGLOBAL && op != OP_CLOSURE && op != OP_CALL) {
            selfRefRegs.erase(A);
            closureRegs.erase(A);
        }
        if (op != OP_LOADINT && op != OP_LOADK && op != OP_ADD && op != OP_SUB && op != OP_MUL &&
            op != OP_ADDI && op != OP_SUBI && op != OP_ADDK && op != OP_MOVE &&
            op != OP_TABLE &&
            op != OP_LTJMP && op != OP_LEJMP && op != OP_GTJMP && op != OP_GEJMP &&
            op != OP_JMP && op != OP_JMPFALSE && op != OP_JMPTRUE && op != OP_LOOP &&
            op != OP_RETURN) {
            knownTypes[A] = KnownType::Unknown;
        }

        // Register allocator: emit prologue at loop start
        auto loopIt = loopAllocMap.find(i);
        if (loopIt != loopAllocMap.end() && loopIt->second.active) {
            bool allInt = true;
            for (int r = 0; r < 256 && allInt; r++)
                if (loopIt->second.regMap[r] >= 0 && knownTypes[r] != KnownType::Int) allInt = false;
            if (allInt) {
                currentLoop = &loopIt->second;
                for (int r = 0; r < 256; r++) {
                    if (currentLoop->regMap[r] < 0) continue;
                    int hw = currentLoop->regMap[r];
                    emitLoadFromMemR(jit.buf, jit.size, hw, r * VAL_SIZE);
                    emitUnboxR(jit.buf, jit.size, hw);
                }
                loopIt->second.nativeBodyStart = jit.size;
            }
        }

        // Register allocator: emit epilogue at loop exit (before exit JMP)
        if (currentLoop && i == currentLoop->exitIdx) {
            // Load TAG_INT once into rcx
            uint8_t movabs_rcx[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs_rcx, 2);
            emit64(jit.buf, jit.size, NB_TAG_INT);
            for (int r = 0; r < 256; r++) {
                if (currentLoop->regMap[r] < 0) continue;
                int hw = currentLoop->regMap[r];
                // mov rax, hw_reg
                uint8_t rex = 0x48; if (hwRegs[hw].extended) rex |= 0x04;
                emit8(jit.buf, jit.size, rex); emit8(jit.buf, jit.size, 0x89);
                emit8(jit.buf, jit.size, 0xC0 | (hwRegs[hw].code << 3));
                // shl rax, 16; shr rax, 16 (clear upper 16 bits = mask payload)
                uint8_t shl16[] = { 0x48, 0xC1, 0xE0, 0x10 };
                emit(jit.buf, jit.size, shl16, 4);
                uint8_t shr16[] = { 0x48, 0xC1, 0xE8, 0x10 };
                emit(jit.buf, jit.size, shr16, 4);
                // or rax, rcx (TAG_INT)
                uint8_t orrr[] = { 0x48, 0x09, 0xc8 };
                emit(jit.buf, jit.size, orrr, 3);
                // mov [rbx + off], rax
                uint8_t store[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, store, 3);
                emit32(jit.buf, jit.size, r * VAL_SIZE);
            }
        }

        switch (op) {
        case OP_LOADK:
            emitLoadK(jit.buf, jit.size, A, &chunk.constants[Bx]);
            if (chunk.constants[Bx].type() == BBL::Type::Int) knownTypes[A] = KnownType::Int;
            else if (chunk.constants[Bx].type() == BBL::Type::Float) knownTypes[A] = KnownType::Float;
            break;
        case OP_LOADINT:
            if (currentLoop && currentLoop->regMap[A] >= 0) {
                emitMovImmR(jit.buf, jit.size, currentLoop->regMap[A], sBx);
            } else {
                emitLoadInt(jit.buf, jit.size, A, sBx);
            }
            knownTypes[A] = KnownType::Int;
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
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0) {
                emitMovRR(jit.buf, jit.size, currentLoop->regMap[A], currentLoop->regMap[B]);
            } else {
                emitMove(jit.buf, jit.size, A, B);
            }
            if (knownTypes[B] == KnownType::Int) knownTypes[A] = KnownType::Int;
            else if (knownTypes[B] == KnownType::Table) knownTypes[A] = KnownType::Table;
            else knownTypes[A] = KnownType::Unknown;
            break;
        case OP_ADD: {
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0 && currentLoop->regMap[C] >= 0) {
                int dA = currentLoop->regMap[A], dB = currentLoop->regMap[B], dC = currentLoop->regMap[C];
                if (dA == dB) { emitAddRR(jit.buf, jit.size, dA, dC); }
                else if (dA == dC) { emitAddRR(jit.buf, jit.size, dA, dB); }
                else { emitMovRR(jit.buf, jit.size, dA, dB); emitAddRR(jit.buf, jit.size, dA, dC); }
                knownTypes[A] = KnownType::Int;
                break;
            }
            if (hasSelfCalls || (knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int)) {
                emitAdd(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Int;
                break;
            }
            if (knownTypes[B] == KnownType::Float && knownTypes[C] == KnownType::Float) {
                emitFAdd(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Float;
                break;
            }
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
            if (currentLoop && currentLoop->regMap[A] >= 0) {
                emitAddImmR(jit.buf, jit.size, currentLoop->regMap[A], sBx);
                break;
            }
            if (knownTypes[A] == KnownType::Int) {
                emitAddi(jit.buf, jit.size, A, sBx);
                break;
            }
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
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0 && currentLoop->regMap[C] >= 0) {
                int dA = currentLoop->regMap[A], dB = currentLoop->regMap[B], dC = currentLoop->regMap[C];
                if (dA == dB) { emitSubRR(jit.buf, jit.size, dA, dC); }
                else { emitMovRR(jit.buf, jit.size, dA, dB); emitSubRR(jit.buf, jit.size, dA, dC); }
                knownTypes[A] = KnownType::Int;
                break;
            }
            if (hasSelfCalls || (knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int)) {
                emitSub(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Int;
                break;
            }
            if (knownTypes[B] == KnownType::Float && knownTypes[C] == KnownType::Float) {
                emitFSub(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Float;
                break;
            }
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
            if (currentLoop && currentLoop->regMap[A] >= 0) {
                emitSubImmR(jit.buf, jit.size, currentLoop->regMap[A], sBx);
            } else {
                emitSubi(jit.buf, jit.size, A, sBx);
            }
            break;
        case OP_MUL:
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0 && currentLoop->regMap[C] >= 0) {
                int dA = currentLoop->regMap[A], dB = currentLoop->regMap[B], dC = currentLoop->regMap[C];
                if (dA == dB) { emitImulRR(jit.buf, jit.size, dA, dC); }
                else if (dA == dC) { emitImulRR(jit.buf, jit.size, dA, dB); }
                else { emitMovRR(jit.buf, jit.size, dA, dB); emitImulRR(jit.buf, jit.size, dA, dC); }
                knownTypes[A] = KnownType::Int;
            } else if (knownTypes[B] == KnownType::Float && knownTypes[C] == KnownType::Float) {
                emitFMul(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Float;
            } else {
                emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((2 << 16) | (B << 8) | C), &errorExitPatches);
                knownTypes[A] = KnownType::Unknown;
            }
            break;

        case OP_LTJMP: {
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0) {
                emitCmpRR(jit.buf, jit.size, currentLoop->regMap[A], currentLoop->regMap[B]);
                uint8_t jcc[] = { 0x0f, 0x8c }; emit(jit.buf, jit.size, jcc, 2);
                patches.push_back({jit.size, i + 2}); emit32(jit.buf, jit.size, 0);
            } else {
                bool direct = knownTypes[A] == KnownType::Int && knownTypes[B] == KnownType::Int;
                size_t p = direct ? emitCompareJmpDirect(jit.buf, jit.size, A, B, 0x8c)
                                  : emitLtjmp(jit.buf, jit.size, A, B);
                patches.push_back({p, i + 2});
            }
            break;
        }
        case OP_LEJMP: {
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0) {
                emitCmpRR(jit.buf, jit.size, currentLoop->regMap[A], currentLoop->regMap[B]);
                uint8_t jcc[] = { 0x0f, 0x8e }; emit(jit.buf, jit.size, jcc, 2);
                patches.push_back({jit.size, i + 2}); emit32(jit.buf, jit.size, 0);
            } else {
                bool direct = knownTypes[A] == KnownType::Int && knownTypes[B] == KnownType::Int;
                size_t p = direct ? emitCompareJmpDirect(jit.buf, jit.size, A, B, 0x8e)
                                  : emitLejmp(jit.buf, jit.size, A, B);
                patches.push_back({p, i + 2});
            }
            break;
        }
        case OP_GTJMP: {
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0) {
                emitCmpRR(jit.buf, jit.size, currentLoop->regMap[A], currentLoop->regMap[B]);
                uint8_t jcc[] = { 0x0f, 0x8f }; emit(jit.buf, jit.size, jcc, 2);
                patches.push_back({jit.size, i + 2}); emit32(jit.buf, jit.size, 0);
            } else {
                bool direct = knownTypes[A] == KnownType::Int && knownTypes[B] == KnownType::Int;
                size_t p = direct ? emitCompareJmpDirect(jit.buf, jit.size, A, B, 0x8f)
                                  : emitGtjmp(jit.buf, jit.size, A, B);
                patches.push_back({p, i + 2});
            }
            break;
        }
        case OP_GEJMP: {
            if (currentLoop && currentLoop->regMap[A] >= 0 && currentLoop->regMap[B] >= 0) {
                emitCmpRR(jit.buf, jit.size, currentLoop->regMap[A], currentLoop->regMap[B]);
                uint8_t jcc[] = { 0x0f, 0x8d }; emit(jit.buf, jit.size, jcc, 2);
                patches.push_back({jit.size, i + 2}); emit32(jit.buf, jit.size, 0);
            } else {
                bool direct = knownTypes[A] == KnownType::Int && knownTypes[B] == KnownType::Int;
                size_t p = direct ? emitCompareJmpDirect(jit.buf, jit.size, A, B, 0x8d)
                                  : emitGejmp(jit.buf, jit.size, A, B);
                patches.push_back({p, i + 2});
            }
            break;
        }

        case OP_JMP: {
            size_t p = emitJmp(jit.buf, jit.size);
            int target = static_cast<int>(i) + 1 + sBx;
            patches.push_back({p, static_cast<size_t>(target)});
            break;
        }
        case OP_LOOP: {
            if (emitStepChecks)
                emitStepCheck(jit.buf, jit.size, stepCountOff, maxStepsOff, errorExitPatches);

            int target = static_cast<int>(i) + 1 - sBx;

            // If in a register-allocated loop, jump back past prologue
            auto laIt = loopAllocMap.find(static_cast<size_t>(target));
            if (laIt != loopAllocMap.end() && laIt->second.active && laIt->second.nativeBodyStart > 0) {
                size_t p = emitJmp(jit.buf, jit.size);
                patchRel32(jit.buf, p, laIt->second.nativeBodyStart);
                currentLoop = nullptr;
                break;
            }

            bool loopHasTable = false;
            bool loopHasFloat = false;
            if (!emitStepChecks) {
                for (int bi = target; bi < static_cast<int>(i); bi++) {
                    uint8_t bop = decodeOP(chunk.code[bi]);
                    if (bop == OP_TABLE) loopHasTable = true;
                    if (bop == OP_DIV) loopHasFloat = true;
                    if (bop == OP_LOADK) {
                        uint16_t kidx = decodeBx(chunk.code[bi]);
                        if (kidx < chunk.constants.size() && chunk.constants[kidx].type() == BBL::Type::Float)
                            loopHasFloat = true;
                    }
                }
            }

            if (!loopHasFloat && !emitStepChecks &&
                !(laIt != loopAllocMap.end() && laIt->second.active)) {
                uint8_t a1[] = { 0x48, 0x89, 0xdf };
                emit(jit.buf, jit.size, a1, 3);
                uint8_t a2[] = { 0x4c, 0x89, 0xe6 };
                emit(jit.buf, jit.size, a2, 3);
                uint8_t a3[] = { 0x4c, 0x89, 0xea };
                emit(jit.buf, jit.size, a3, 3);
                uint8_t a4[] = { 0xb9 };
                emit(jit.buf, jit.size, a4, 1);
                emit32(jit.buf, jit.size, static_cast<uint32_t>(i));
                uint8_t movabs[] = { 0x48, 0xb8 };
                emit(jit.buf, jit.size, movabs, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitLoopTrace));
                uint8_t call[] = { 0xff, 0xd0 };
                emit(jit.buf, jit.size, call, 2);
                // Return: -1 = warmup (continue per-fn JIT loop),
                // 1 = trace ran the loop (skip backward jump)
                uint8_t cmp0[] = { 0x48, 0x83, 0xf8, 0x00 };  // cmp rax, 0
                emit(jit.buf, jit.size, cmp0, 4);
                uint8_t jg[] = { 0x0f, 0x8f };  // jg rel32 (rax > 0 → skip loop)
                emit(jit.buf, jit.size, jg, 2);
                size_t skipPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                size_t p = emitJmp(jit.buf, jit.size);
                patchRel32(jit.buf, p, nativeOffsets[target]);
                patchRel32(jit.buf, skipPatch, jit.size);
            } else {
                size_t p = emitJmp(jit.buf, jit.size);
                patchRel32(jit.buf, p, nativeOffsets[target]);
            }
            break;
        }

        case OP_RETURN:
            emitReturn(jit.buf, jit.size, A);
            break;

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            if (self && state.vm) {
                BblValue* gv = state.vm->getGlobal(symId);
                if (gv && gv->type() == BBL::Type::Fn &&
                    gv->isClosure() && gv->closureVal() == self) {
                    selfRefRegs.insert(A);
                    hasSelfCalls = true;
                    break;
                }
                if (gv && gv->type() == BBL::Type::Fn && gv->isClosure()) {
                    closureRegs[A] = gv->closureVal();
                }
            }
            if (symId < state.vm->globalsFlat.size()) {
                // Inline: load vm ptr from [r12+112], load globalsFlat.data() from [vm+8288],
                // load value from [data + symId*8], check non-zero, store to regs[A]
                uint8_t load_vm[] = { 0x49, 0x8b, 0x84, 0x24 }; // mov rax, [r12+112]
                emit(jit.buf, jit.size, load_vm, 4);
                emit32(jit.buf, jit.size, static_cast<uint32_t>(vmOff));
                uint8_t load_flat[] = { 0x48, 0x8b, 0x80 }; // mov rax, [rax+8288]
                emit(jit.buf, jit.size, load_flat, 3);
                emit32(jit.buf, jit.size, static_cast<uint32_t>(globalsFlatOff));
                uint8_t load_val[] = { 0x48, 0x8b, 0x80 }; // mov rax, [rax+symId*8]
                emit(jit.buf, jit.size, load_val, 3);
                emit32(jit.buf, jit.size, symId * VAL_SIZE);
                // test rax, rax; jz fallback
                uint8_t test[] = { 0x48, 0x85, 0xc0 };
                emit(jit.buf, jit.size, test, 3);
                uint8_t jz[] = { 0x74 };
                emit(jit.buf, jit.size, jz, 1);
                size_t fallbackPatch = jit.size;
                emit8(jit.buf, jit.size, 0);
                // Store to regs[A]
                uint8_t store[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, store, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                // jmp done
                uint8_t jmpDone[] = { 0xeb };
                emit(jit.buf, jit.size, jmpDone, 1);
                size_t donePatch = jit.size;
                emit8(jit.buf, jit.size, 0);
                // fallback: call C helper
                jit.buf[fallbackPatch] = static_cast<uint8_t>(jit.size - fallbackPatch - 1);
                emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A, &errorExitPatches);
                jit.buf[donePatch] = static_cast<uint8_t>(jit.size - donePatch - 1);
            } else {
                emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A, &errorExitPatches);
            }
            break;
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetGlobal, symId, A, &errorExitPatches);
            break;
        }
        case OP_ENVGET: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            if (self && self->env) {
                auto nit = state.symbolNames.find(symId);
                if (nit != state.symbolNames.end()) {
                    auto v = self->env->get(BblValue::makeString(nit->second));
                    if (v && v->type() == BBL::Type::Fn && v->isClosure() && v->closureVal() == self) {
                        selfRefRegs.insert(A);
                        hasSelfCalls = true;
                        break;
                    }
                    if (v && v->type() == BBL::Type::Fn && v->isClosure()) {
                        closureRegs[A] = v->closureVal();
                    }
                }
            }
            emitCallHelper2(jit.buf, jit.size, (void*)jitEnvGet, symId, A, &errorExitPatches);
            break;
        }
        case OP_ENVSET: {
            uint32_t symId = static_cast<uint32_t>(chunk.constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitEnvSet, symId, A, &errorExitPatches);
            break;
        }
        case OP_CALL: {
            if (emitStepChecks)
                emitStepCheck(jit.buf, jit.size, stepCountOff, maxStepsOff, errorExitPatches);
            if (selfRefRegs.count(A)) {
                // Light self-recursive call: only save/restore rbx
                // lea rdi, [rbx + A*8]
                uint8_t lea[] = { 0x48, 0x8d, 0xbb };
                emit(jit.buf, jit.size, lea, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                // call rel32 → light entry
                emit8(jit.buf, jit.size, 0xe8);
                selfCallPatches.push_back(jit.size);
                emit32(jit.buf, jit.size, 0);
                if (emitStepChecks)
                    emitErrorCheck(jit.buf, jit.size, errorExitPatches);
                // result in rax → R[A]
                uint8_t st1[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, st1, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
            } else {
            // Try to inline: check if base register holds a known closure
            auto cit = closureRegs.find(A);
            if (cit != closureRegs.end() && B == cit->second->arity) {
                BblClosure* callee = cit->second;
                
                // Guard: verify callee pointer matches at runtime
                // mov rax, [rbx+A*8]; shl rax, 16; shr rax, 16; movabs rcx, expected; cmp rax, rcx; jne slow
                uint8_t ldcl[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ldcl, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t shl16cl[] = { 0x48, 0xc1, 0xe0, 0x10 };
                emit(jit.buf, jit.size, shl16cl, 4);
                uint8_t shr16cl[] = { 0x48, 0xc1, 0xe8, 0x10 };
                emit(jit.buf, jit.size, shr16cl, 4);
                uint8_t movabscl[] = { 0x48, 0xb9 };
                emit(jit.buf, jit.size, movabscl, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(callee));
                uint8_t cmpcl[] = { 0x48, 0x39, 0xc8 };
                emit(jit.buf, jit.size, cmpcl, 3);
                uint8_t jnecl[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jnecl, 2);
                size_t guardPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

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
                    case OP_ADD: emitAdd(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); knownTypes[remap(cA)] = KnownType::Int; break;
                    case OP_SUB: emitSub(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); knownTypes[remap(cA)] = KnownType::Int; break;
                    case OP_MUL: emitMul(jit.buf, jit.size, remap(cA), remap(cB), remap(cC)); knownTypes[remap(cA)] = KnownType::Int; break;
                    case OP_ADDI: emitAddi(jit.buf, jit.size, remap(cA), csBx); knownTypes[remap(cA)] = KnownType::Int; break;
                    case OP_SUBI: emitSubi(jit.buf, jit.size, remap(cA), csBx); knownTypes[remap(cA)] = KnownType::Int; break;
                    case OP_MOVE: emitMove(jit.buf, jit.size, remap(cA), remap(cB)); knownTypes[remap(cA)] = knownTypes[remap(cB)]; break;
                    case OP_LOADINT: emitLoadInt(jit.buf, jit.size, remap(cA), csBx); knownTypes[remap(cA)] = KnownType::Int; break;
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
                uint8_t jmpDoneCl[] = { 0xe9 };
                emit(jit.buf, jit.size, jmpDoneCl, 1);
                size_t inlineDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Guard miss: fall to C helper
                patchRel32(jit.buf, guardPatch, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitCall, A, B, &errorExitPatches);
                patchRel32(jit.buf, inlineDonePatch, jit.size);
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

        case OP_EQ:  { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x94); break; }
        case OP_NEQ: { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x95); break; }
        case OP_LT:  { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x9c); break; }
        case OP_GT:  { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x9f); break; }
        case OP_LTE: { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x9e); break; }
        case OP_GTE: { bool d = knownTypes[B] == KnownType::Int && knownTypes[C] == KnownType::Int; (d ? emitCmpDirect : emitCmp)(jit.buf, jit.size, A, B, C, 0x9d); break; }

        case OP_DIV:
            if (knownTypes[B] == KnownType::Float && knownTypes[C] == KnownType::Float) {
                emitFDiv(jit.buf, jit.size, A, B, C);
                knownTypes[A] = KnownType::Float;
            } else {
                emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((3 << 16) | (B << 8) | C), &errorExitPatches);
            }
            break;
        case OP_MOD:
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((4 << 16) | (B << 8) | C), &errorExitPatches);
            break;

        case OP_TABLE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitTable, A, B, &errorExitPatches);
            knownTypes[A] = KnownType::Table;
            break;

        case OP_MCALL: {
            BblString* methodStr = chunk.constants[C].stringVal();
            bool knownTable = (knownTypes[A] == KnownType::Table);

            if (methodStr == state.m.get) {
                // Inline table get: type guard → hash probe → load value
                size_t slowPatch = 0, gcPatch = 0;

                if (!knownTable) {
                    // Type guard: check TAG_OBJECT
                    uint8_t ld[] = { 0x48, 0x8b, 0x83 };
                    emit(jit.buf, jit.size, ld, 3);
                    emit32(jit.buf, jit.size, A * VAL_SIZE); // mov rax, [rbx+A*8]
                    uint8_t movcx[] = { 0x48, 0x89, 0xc1 };
                    emit(jit.buf, jit.size, movcx, 3); // mov rcx, rax
                    uint8_t shrcx[] = { 0x48, 0xc1, 0xe9, 0x30 };
                    emit(jit.buf, jit.size, shrcx, 4); // shr rcx, 48
                    uint8_t cmpw[] = { 0x66, 0x81, 0xf9 };
                    emit(jit.buf, jit.size, cmpw, 3);
                    emit16(jit.buf, jit.size, 0xFFFF); // cmp cx, 0xFFFF (TAG_OBJECT)
                    uint8_t jne[] = { 0x0f, 0x85 };
                    emit(jit.buf, jit.size, jne, 2);
                    slowPatch = jit.size;
                    emit32(jit.buf, jit.size, 0);
                    // Extract pointer: shl rax, 16; shr rax, 16
                    uint8_t shl16[] = { 0x48, 0xc1, 0xe0, 0x10 };
                    emit(jit.buf, jit.size, shl16, 4);
                    uint8_t shr16[] = { 0x48, 0xc1, 0xe8, 0x10 };
                    emit(jit.buf, jit.size, shr16, 4); // rax = table ptr
                    // Check gcType == Table
                    uint8_t cmpb[] = { 0x80, 0x38 };
                    emit(jit.buf, jit.size, cmpb, 2);
                    emit8(jit.buf, jit.size, tableGcType); // cmp byte [rax], tableGcType
                    emit(jit.buf, jit.size, jne, 2);
                    gcPatch = jit.size;
                    emit32(jit.buf, jit.size, 0);
                    uint8_t movrdi[] = { 0x48, 0x89, 0xc7 };
                    emit(jit.buf, jit.size, movrdi, 3); // mov rdi, rax
                } else {
                    // Known table: extract pointer directly
                    uint8_t ld[] = { 0x48, 0x8b, 0xbb };
                    emit(jit.buf, jit.size, ld, 3);
                    emit32(jit.buf, jit.size, A * VAL_SIZE); // mov rdi, [rbx+A*8]
                    uint8_t shl16[] = { 0x48, 0xc1, 0xe7, 0x10 };
                    emit(jit.buf, jit.size, shl16, 4);
                    uint8_t shr16[] = { 0x48, 0xc1, 0xef, 0x10 };
                    emit(jit.buf, jit.size, shr16, 4); // rdi = table ptr
                }

                // Array fast path: check if key is integer in [0, asize)
                // Load key: mov rsi, [rbx + (A+1)*8]
                uint8_t ldkey_arr[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey_arr, 3);
                emit32(jit.buf, jit.size, (A + 1) * VAL_SIZE);
                // Check TAG_INT: shr rsi copy, 48; cmp with 0xFFFA
                uint8_t movrax_rsi_a[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi_a, 3); // mov rax, rsi
                uint8_t shrrax48[] = { 0x48, 0xc1, 0xe8, 0x30 };
                emit(jit.buf, jit.size, shrrax48, 4); // shr rax, 48
                uint8_t cmpax[] = { 0x66, 0x3d };
                emit(jit.buf, jit.size, cmpax, 2);
                emit16(jit.buf, jit.size, 0xFFFA); // cmp ax, TAG_INT>>48
                uint8_t jne_rel32_a[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_rel32_a, 2);
                size_t arrSkipPatch = jit.size;
                emit32(jit.buf, jit.size, 0); // jne → hash_probe
                // Extract int: shl rsi, 16; sar rsi, 16
                uint8_t shlrsi[] = { 0x48, 0xc1, 0xe6, 0x10 };
                emit(jit.buf, jit.size, shlrsi, 4);
                uint8_t sarrsi[] = { 0x48, 0xc1, 0xfe, 0x10 };
                emit(jit.buf, jit.size, sarrsi, 4); // rsi = int value
                // Check >= 0: test rsi, rsi; js → hash
                uint8_t testrsi[] = { 0x48, 0x85, 0xf6 };
                emit(jit.buf, jit.size, testrsi, 3);
                uint8_t js_rel32[] = { 0x0f, 0x88 };
                emit(jit.buf, jit.size, js_rel32, 2);
                size_t arrNegPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // cmp esi, [rdi + tblAsizeOff]
                uint8_t cmpesi[] = { 0x3b, 0xb7 };
                emit(jit.buf, jit.size, cmpesi, 2);
                emit32(jit.buf, jit.size, tblAsizeOff);
                // jae → hash
                uint8_t jae_rel32[] = { 0x0f, 0x83 };
                emit(jit.buf, jit.size, jae_rel32, 2);
                size_t arrOobPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Load array: mov rcx, [rdi + tblArrayOff]
                uint8_t ldArr[] = { 0x48, 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldArr, 3);
                emit32(jit.buf, jit.size, tblArrayOff);
                // Load value: mov rax, [rcx + rsi*8]
                uint8_t ldArrVal[] = { 0x48, 0x8b, 0x04, 0xf1 };
                emit(jit.buf, jit.size, ldArrVal, 4);
                // test rax, rax; jz → hash (empty slot)
                uint8_t testrax[] = { 0x48, 0x85, 0xc0 };
                emit(jit.buf, jit.size, testrax, 3);
                uint8_t jz_rel32[] = { 0x0f, 0x84 };
                emit(jit.buf, jit.size, jz_rel32, 2);
                size_t arrEmptyPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Store result: mov [rbx+A*8], rax; jmp done
                uint8_t stval_a[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval_a, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t jmprel32_a[] = { 0xe9 };
                emit(jit.buf, jit.size, jmprel32_a, 1);
                size_t arrDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // hash_probe: (patch array miss jumps here)
                patchRel32(jit.buf, arrSkipPatch, jit.size);
                patchRel32(jit.buf, arrNegPatch, jit.size);
                patchRel32(jit.buf, arrOobPatch, jit.size);
                patchRel32(jit.buf, arrEmptyPatch, jit.size);

                // Load capacity: mov ecx, [rdi + tblCapacityOff]
                uint8_t ldcap[] = { 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldcap, 2);
                emit32(jit.buf, jit.size, tblCapacityOff);
                // test ecx, ecx; jz slow_path
                uint8_t testecx[] = { 0x85, 0xc9 };
                emit(jit.buf, jit.size, testecx, 2);
                uint8_t jz[] = { 0x0f, 0x84 };
                emit(jit.buf, jit.size, jz, 2);
                size_t capPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // Load buckets: mov rdx, [rdi + tblBucketsOff]
                uint8_t ldbkt[] = { 0x48, 0x8b, 0x97 };
                emit(jit.buf, jit.size, ldbkt, 3);
                emit32(jit.buf, jit.size, tblBucketsOff);
                // Load key: mov rsi, [rbx + (A+1)*8]
                uint8_t ldkey[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey, 3);
                emit32(jit.buf, jit.size, (A + 1) * VAL_SIZE);

                // Hash: mov rax, rsi; movabs r8, GOLDEN; imul rax, r8
                uint8_t movrax_rsi[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi, 3);
                uint8_t movabs_r8[] = { 0x49, 0xb8 };
                emit(jit.buf, jit.size, movabs_r8, 2);
                emit64(jit.buf, jit.size, 11400714819323198485ULL);
                uint8_t imul[] = { 0x49, 0x0f, 0xaf, 0xc0 };
                emit(jit.buf, jit.size, imul, 4); // imul rax, r8

                // Index: sub ecx, 1; and eax, ecx
                uint8_t subecx[] = { 0x83, 0xe9, 0x01 };
                emit(jit.buf, jit.size, subecx, 3);
                uint8_t andeax[] = { 0x21, 0xc8 };
                emit(jit.buf, jit.size, andeax, 2);

                // Probe: shl rax, 4; add rdx, rax
                uint8_t shlrax4[] = { 0x48, 0xc1, 0xe0, 0x04 };
                emit(jit.buf, jit.size, shlrax4, 4);
                uint8_t addrdx[] = { 0x48, 0x01, 0xc2 };
                emit(jit.buf, jit.size, addrdx, 3); // rdx = &entry

                // Compare: cmp [rdx], rsi (entry.key vs search key)
                uint8_t cmprdxrsi[] = { 0x48, 0x39, 0x32 };
                emit(jit.buf, jit.size, cmprdxrsi, 3);
                // je found
                uint8_t je[] = { 0x74 };
                emit(jit.buf, jit.size, je, 1);
                size_t foundPatch = jit.size;
                emit8(jit.buf, jit.size, 0); // rel8

                // Check empty: cmp qword [rdx], 0
                uint8_t cmpq0[] = { 0x48, 0x83, 0x3a, 0x00 };
                emit(jit.buf, jit.size, cmpq0, 4);
                // je not_found
                emit(jit.buf, jit.size, je, 1);
                size_t notFoundPatch = jit.size;
                emit8(jit.buf, jit.size, 0); // rel8

                // Collision → slow path
                uint8_t jmprel32[] = { 0xe9 };
                emit(jit.buf, jit.size, jmprel32, 1);
                size_t collisionPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // found: load value
                jit.buf[foundPatch] = static_cast<uint8_t>(jit.size - foundPatch - 1);
                uint8_t ldval[] = { 0x48, 0x8b, 0x42, 0x08 };
                emit(jit.buf, jit.size, ldval, 4); // mov rax, [rdx+8]
                uint8_t stval[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE); // mov [rbx+A*8], rax
                emit(jit.buf, jit.size, jmprel32, 1);
                size_t donePatch1 = jit.size;
                emit32(jit.buf, jit.size, 0);

                // not_found → also fall to slow path (key might be in array part)
                jit.buf[notFoundPatch] = static_cast<uint8_t>(jit.size - notFoundPatch - 1);
                emit(jit.buf, jit.size, jmprel32, 1);
                size_t notFoundSlowPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // slow_path: call C helper
                if (!knownTable) {
                    patchRel32(jit.buf, slowPatch, jit.size);
                    patchRel32(jit.buf, gcPatch, jit.size);
                }
                patchRel32(jit.buf, capPatch, jit.size);
                patchRel32(jit.buf, collisionPatch, jit.size);
                patchRel32(jit.buf, notFoundSlowPatch, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitTableGet, A, B, &errorExitPatches);

                // done:
                patchRel32(jit.buf, donePatch1, jit.size);
                patchRel32(jit.buf, arrDonePatch, jit.size);

            } else if (methodStr == state.m.set) {
                // Inline table set: type guard → hash probe → update value (existing keys only)
                size_t slowPatch = 0, gcPatch = 0;

                if (!knownTable) {
                    uint8_t ld[] = { 0x48, 0x8b, 0x83 };
                    emit(jit.buf, jit.size, ld, 3);
                    emit32(jit.buf, jit.size, A * VAL_SIZE);
                    uint8_t movcx[] = { 0x48, 0x89, 0xc1 };
                    emit(jit.buf, jit.size, movcx, 3);
                    uint8_t shrcx[] = { 0x48, 0xc1, 0xe9, 0x30 };
                    emit(jit.buf, jit.size, shrcx, 4);
                    uint8_t cmpw[] = { 0x66, 0x81, 0xf9 };
                    emit(jit.buf, jit.size, cmpw, 3);
                    emit16(jit.buf, jit.size, 0xFFFF);
                    uint8_t jne[] = { 0x0f, 0x85 };
                    emit(jit.buf, jit.size, jne, 2);
                    slowPatch = jit.size;
                    emit32(jit.buf, jit.size, 0);
                    uint8_t shl16[] = { 0x48, 0xc1, 0xe0, 0x10 };
                    emit(jit.buf, jit.size, shl16, 4);
                    uint8_t shr16[] = { 0x48, 0xc1, 0xe8, 0x10 };
                    emit(jit.buf, jit.size, shr16, 4);
                    uint8_t cmpb[] = { 0x80, 0x38 };
                    emit(jit.buf, jit.size, cmpb, 2);
                    emit8(jit.buf, jit.size, tableGcType);
                    emit(jit.buf, jit.size, jne, 2);
                    gcPatch = jit.size;
                    emit32(jit.buf, jit.size, 0);
                    uint8_t movrdi[] = { 0x48, 0x89, 0xc7 };
                    emit(jit.buf, jit.size, movrdi, 3);
                } else {
                    uint8_t ld[] = { 0x48, 0x8b, 0xbb };
                    emit(jit.buf, jit.size, ld, 3);
                    emit32(jit.buf, jit.size, A * VAL_SIZE);
                    uint8_t shl16[] = { 0x48, 0xc1, 0xe7, 0x10 };
                    emit(jit.buf, jit.size, shl16, 4);
                    uint8_t shr16[] = { 0x48, 0xc1, 0xef, 0x10 };
                    emit(jit.buf, jit.size, shr16, 4);
                }

                // Array fast path for set: check if key is integer in [0, asize)
                uint8_t ldkey_s[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey_s, 3);
                emit32(jit.buf, jit.size, (A + 1) * VAL_SIZE); // mov rsi, key
                uint8_t movrax_rsi_s[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi_s, 3);
                uint8_t shrrax48_s[] = { 0x48, 0xc1, 0xe8, 0x30 };
                emit(jit.buf, jit.size, shrrax48_s, 4);
                uint8_t cmpax_s[] = { 0x66, 0x3d };
                emit(jit.buf, jit.size, cmpax_s, 2);
                emit16(jit.buf, jit.size, 0xFFFA);
                uint8_t jne_s[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_s, 2);
                size_t arrSkipSetPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Extract int
                uint8_t shlrsi_s[] = { 0x48, 0xc1, 0xe6, 0x10 };
                emit(jit.buf, jit.size, shlrsi_s, 4);
                uint8_t sarrsi_s[] = { 0x48, 0xc1, 0xfe, 0x10 };
                emit(jit.buf, jit.size, sarrsi_s, 4);
                // Bounds check
                uint8_t testrsi_s[] = { 0x48, 0x85, 0xf6 };
                emit(jit.buf, jit.size, testrsi_s, 3);
                uint8_t js_s[] = { 0x0f, 0x88 };
                emit(jit.buf, jit.size, js_s, 2);
                size_t arrNegSetPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                uint8_t cmpesi_s[] = { 0x3b, 0xb7 };
                emit(jit.buf, jit.size, cmpesi_s, 2);
                emit32(jit.buf, jit.size, tblAsizeOff);
                uint8_t jae_s[] = { 0x0f, 0x83 };
                emit(jit.buf, jit.size, jae_s, 2);
                size_t arrOobSetPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Load array ptr: mov rcx, [rdi + tblArrayOff]
                uint8_t ldArr_s[] = { 0x48, 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldArr_s, 3);
                emit32(jit.buf, jit.size, tblArrayOff);
                // Check if slot empty: cmp qword [rcx+rsi*8], 0
                uint8_t cmpArrSlot[] = { 0x48, 0x83, 0x3c, 0xf1, 0x00 };
                emit(jit.buf, jit.size, cmpArrSlot, 5);
                // je → new_key (need to increment count)
                uint8_t je_newkey[] = { 0x74 };
                emit(jit.buf, jit.size, je_newkey, 1);
                size_t newKeyPatch = jit.size;
                emit8(jit.buf, jit.size, 0); // rel8
                // Existing key: just store value
                uint8_t ldnewval_s[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ldnewval_s, 3);
                emit32(jit.buf, jit.size, (A + 2) * VAL_SIZE);
                uint8_t stArrVal[] = { 0x48, 0x89, 0x04, 0xf1 };
                emit(jit.buf, jit.size, stArrVal, 4); // mov [rcx+rsi*8], rax
                emitWriteBarrier(jit.buf, jit.size);
                uint8_t jmp_done_s[] = { 0xe9 };
                emit(jit.buf, jit.size, jmp_done_s, 1);
                size_t existDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0); // rel32
                // new_key: store value + increment count
                jit.buf[newKeyPatch] = static_cast<uint8_t>(jit.size - newKeyPatch - 1);
                emit(jit.buf, jit.size, ldnewval_s, 3);
                emit32(jit.buf, jit.size, (A + 2) * VAL_SIZE);
                emit(jit.buf, jit.size, stArrVal, 4); // mov [rcx+rsi*8], rax
                emitWriteBarrier(jit.buf, jit.size);
                // inc dword [rdi + tblCountOff]
                uint8_t incCount[] = { 0xff, 0x87 };
                emit(jit.buf, jit.size, incCount, 2);
                emit32(jit.buf, jit.size, tblCountOff);
                // done:
                patchRel32(jit.buf, existDonePatch, jit.size);
                // Store null result
                uint8_t movabs_null_s[] = { 0x48, 0xb8 };
                emit(jit.buf, jit.size, movabs_null_s, 2);
                emit64(jit.buf, jit.size, NB_TAG_NULL);
                uint8_t stval_s[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval_s, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t jmprel32_s[] = { 0xe9 };
                emit(jit.buf, jit.size, jmprel32_s, 1);
                size_t arrDoneSetPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // hash_probe: patch array misses
                patchRel32(jit.buf, arrSkipSetPatch, jit.size);
                patchRel32(jit.buf, arrNegSetPatch, jit.size);
                patchRel32(jit.buf, arrOobSetPatch, jit.size);

                // Hash probe (same as get)
                uint8_t ldcap[] = { 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldcap, 2);
                emit32(jit.buf, jit.size, tblCapacityOff);
                uint8_t testecx[] = { 0x85, 0xc9 };
                emit(jit.buf, jit.size, testecx, 2);
                uint8_t jz[] = { 0x0f, 0x84 };
                emit(jit.buf, jit.size, jz, 2);
                size_t capPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                uint8_t ldbkt[] = { 0x48, 0x8b, 0x97 };
                emit(jit.buf, jit.size, ldbkt, 3);
                emit32(jit.buf, jit.size, tblBucketsOff);
                uint8_t ldkey[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey, 3);
                emit32(jit.buf, jit.size, (A + 1) * VAL_SIZE);

                uint8_t movrax_rsi[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi, 3);
                uint8_t movabs_r8[] = { 0x49, 0xb8 };
                emit(jit.buf, jit.size, movabs_r8, 2);
                emit64(jit.buf, jit.size, 11400714819323198485ULL);
                uint8_t imulr8[] = { 0x49, 0x0f, 0xaf, 0xc0 };
                emit(jit.buf, jit.size, imulr8, 4);

                uint8_t subecx[] = { 0x83, 0xe9, 0x01 };
                emit(jit.buf, jit.size, subecx, 3);
                uint8_t andeax[] = { 0x21, 0xc8 };
                emit(jit.buf, jit.size, andeax, 2);
                uint8_t shlrax4[] = { 0x48, 0xc1, 0xe0, 0x04 };
                emit(jit.buf, jit.size, shlrax4, 4);
                uint8_t addrdx[] = { 0x48, 0x01, 0xc2 };
                emit(jit.buf, jit.size, addrdx, 3);

                // Compare key
                uint8_t cmprdxrsi[] = { 0x48, 0x39, 0x32 };
                emit(jit.buf, jit.size, cmprdxrsi, 3);
                uint8_t jne_rel32[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_rel32, 2);
                size_t missSetPatch = jit.size;
                emit32(jit.buf, jit.size, 0);

                // Hit: store new value at entry.val
                uint8_t ldnewval[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ldnewval, 3);
                emit32(jit.buf, jit.size, (A + 2) * VAL_SIZE); // mov rax, [rbx+(A+2)*8]
                uint8_t stentry[] = { 0x48, 0x89, 0x42, 0x08 };
                emit(jit.buf, jit.size, stentry, 4); // mov [rdx+8], rax
                // Store null result
                uint8_t movabs_null[] = { 0x48, 0xb8 };
                emit(jit.buf, jit.size, movabs_null, 2);
                emit64(jit.buf, jit.size, NB_TAG_NULL);
                uint8_t stval[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t jmprel32[] = { 0xe9 };
                emit(jit.buf, jit.size, jmprel32, 1);
                size_t donePatchSet = jit.size;
                emit32(jit.buf, jit.size, 0);

                // slow_path
                if (!knownTable) {
                    patchRel32(jit.buf, slowPatch, jit.size);
                    patchRel32(jit.buf, gcPatch, jit.size);
                }
                patchRel32(jit.buf, capPatch, jit.size);
                patchRel32(jit.buf, missSetPatch, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitTableSet, A, B, &errorExitPatches);

                patchRel32(jit.buf, donePatchSet, jit.size);
                patchRel32(jit.buf, arrDoneSetPatch, jit.size);

            } else if (methodStr == state.m.at) {
                // Inline vector:at for integer vectors (elemSize==8)
                // Type guard: check TAG_OBJECT + gcType == Vec
                uint8_t ld_v[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ld_v, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE); // mov rax, [rbx+A*8]
                uint8_t movcx_v[] = { 0x48, 0x89, 0xc1 };
                emit(jit.buf, jit.size, movcx_v, 3);
                uint8_t shrcx_v[] = { 0x48, 0xc1, 0xe9, 0x30 };
                emit(jit.buf, jit.size, shrcx_v, 4);
                uint8_t cmpw_v[] = { 0x66, 0x81, 0xf9 };
                emit(jit.buf, jit.size, cmpw_v, 3);
                emit16(jit.buf, jit.size, 0xFFFF); // TAG_OBJECT
                uint8_t jne_v[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_v, 2);
                size_t vecSlowPatch1 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Extract pointer
                uint8_t shl16_v[] = { 0x48, 0xc1, 0xe0, 0x10 };
                emit(jit.buf, jit.size, shl16_v, 4);
                uint8_t shr16_v[] = { 0x48, 0xc1, 0xe8, 0x10 };
                emit(jit.buf, jit.size, shr16_v, 4); // rax = vec ptr
                // Check gcType == Vec
                uint8_t cmpb_v[] = { 0x80, 0x38 };
                emit(jit.buf, jit.size, cmpb_v, 2);
                emit8(jit.buf, jit.size, vecGcType);
                emit(jit.buf, jit.size, jne_v, 2);
                size_t vecSlowPatch2 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // rax = BblVec*. Check elemTypeTag == Int (BBL::Type is 4 bytes)
                uint8_t cmpElemType[] = { 0x83, 0xb8 };
                emit(jit.buf, jit.size, cmpElemType, 2);
                emit32(jit.buf, jit.size, vecElemTypeTagOff);
                emit8(jit.buf, jit.size, vecIntTypeTag); // cmp dword [rax+off], Int
                emit(jit.buf, jit.size, jne_v, 2);
                size_t vecSlowPatch3 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // mov rdi, rax (save vec ptr)
                uint8_t movrdi_v[] = { 0x48, 0x89, 0xc7 };
                emit(jit.buf, jit.size, movrdi_v, 3);
                // Unbox index: mov rsi, [rbx+(A+1)*8]
                uint8_t ldidx[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldidx, 3);
                emit32(jit.buf, jit.size, (A + 1) * VAL_SIZE);
                uint8_t shlrsi_v[] = { 0x48, 0xc1, 0xe6, 0x10 };
                emit(jit.buf, jit.size, shlrsi_v, 4);
                uint8_t sarrsi_v[] = { 0x48, 0xc1, 0xfe, 0x10 };
                emit(jit.buf, jit.size, sarrsi_v, 4); // rsi = index
                // Bounds check: compute count = (end - begin) / 8
                // Load end ptr: mov rcx, [rdi+vecDataSizeOff] (actually end ptr)
                uint8_t ldDataEnd[] = { 0x48, 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldDataEnd, 3);
                emit32(jit.buf, jit.size, vecDataSizeOff);
                // Load begin ptr: mov rdx, [rdi+vecDataPtrOff]  (reuse rdx temp)
                uint8_t ldDataBegin[] = { 0x48, 0x8b, 0x97 };
                emit(jit.buf, jit.size, ldDataBegin, 3);
                emit32(jit.buf, jit.size, vecDataPtrOff);
                // sub rcx, rdx → byte count
                uint8_t subrcx_rdx[] = { 0x48, 0x29, 0xd1 };
                emit(jit.buf, jit.size, subrcx_rdx, 3);
                // shr rcx, 3 (divide by 8 = elemSize for int vectors)
                uint8_t shrcx3[] = { 0x48, 0xc1, 0xe9, 0x03 };
                emit(jit.buf, jit.size, shrcx3, 4);
                // cmp rsi, rcx (unsigned)
                uint8_t cmprsi_rcx[] = { 0x48, 0x39, 0xce };
                emit(jit.buf, jit.size, cmprsi_rcx, 3);
                // jae slow (out of bounds)
                uint8_t jae_v[] = { 0x0f, 0x83 };
                emit(jit.buf, jit.size, jae_v, 2);
                size_t vecSlowPatch4 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Load data pointer: rdx already has begin ptr from bounds check
                // Load element: mov rax, [rdx+rsi*8] (raw int64)
                uint8_t ldElem[] = { 0x48, 0x8b, 0x04, 0xf2 };
                emit(jit.buf, jit.size, ldElem, 4);
                // NaN-box as int: and rax, PAYLOAD_MASK; or rax, TAG_INT
                // Use the compact shl/shr + or pattern
                emit(jit.buf, jit.size, shl16_v, 4); // shl rax, 16
                emit(jit.buf, jit.size, shr16_v, 4); // shr rax, 16
                uint8_t movabs_tag[] = { 0x48, 0xb9 };
                emit(jit.buf, jit.size, movabs_tag, 2);
                emit64(jit.buf, jit.size, NB_TAG_INT);
                uint8_t orrax_rcx[] = { 0x48, 0x09, 0xc8 };
                emit(jit.buf, jit.size, orrax_rcx, 3);
                // Store result
                uint8_t stval_v[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval_v, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t jmp_v[] = { 0xe9 };
                emit(jit.buf, jit.size, jmp_v, 1);
                size_t vecDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // slow path: generic mcall
                patchRel32(jit.buf, vecSlowPatch1, jit.size);
                patchRel32(jit.buf, vecSlowPatch2, jit.size);
                patchRel32(jit.buf, vecSlowPatch3, jit.size);
                patchRel32(jit.buf, vecSlowPatch4, jit.size);
                {
                    uint8_t a1[] = { 0x48, 0x89, 0xdf }; emit(jit.buf, jit.size, a1, 3);
                    uint8_t a2[] = { 0x4c, 0x89, 0xe6 }; emit(jit.buf, jit.size, a2, 3);
                    uint8_t a3[] = { 0xba }; emit(jit.buf, jit.size, a3, 1); emit32(jit.buf, jit.size, A);
                    uint8_t a4[] = { 0xb9 }; emit(jit.buf, jit.size, a4, 1); emit32(jit.buf, jit.size, B);
                    uint8_t movr8[] = { 0x49, 0xb8 }; emit(jit.buf, jit.size, movr8, 2);
                    emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(methodStr));
                    uint8_t movabs[] = { 0x48, 0xb8 }; emit(jit.buf, jit.size, movabs, 2);
                    emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitMcall));
                    uint8_t call[] = { 0xff, 0xd0 }; emit(jit.buf, jit.size, call, 2);
                    emitErrorCheck(jit.buf, jit.size, errorExitPatches);
                }
                patchRel32(jit.buf, vecDonePatch, jit.size);

            } else if (methodStr == state.m.length) {
                // Inline string:length: check TAG_STRING, extract ptr, load size, NaN-box
                uint8_t ld_len[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ld_len, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE); // mov rax, [rbx+A*8]
                uint8_t movcx_len[] = { 0x48, 0x89, 0xc1 };
                emit(jit.buf, jit.size, movcx_len, 3); // mov rcx, rax
                uint8_t shrcx_len[] = { 0x48, 0xc1, 0xe9, 0x30 };
                emit(jit.buf, jit.size, shrcx_len, 4); // shr rcx, 48
                uint8_t cmpw_len[] = { 0x66, 0x81, 0xf9 };
                emit(jit.buf, jit.size, cmpw_len, 3);
                emit16(jit.buf, jit.size, 0xFFFE); // TAG_STRING upper bits
                uint8_t jne_len[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_len, 2);
                size_t lenSlowPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Extract pointer: shl rax, 16; shr rax, 16
                uint8_t shl16_len[] = { 0x48, 0xc1, 0xe0, 0x10 };
                emit(jit.buf, jit.size, shl16_len, 4);
                uint8_t shr16_len[] = { 0x48, 0xc1, 0xe8, 0x10 };
                emit(jit.buf, jit.size, shr16_len, 4);
                // Load std::string size: the "end" pointer approach
                // Actually use strSizeOff which was computed from dummy
                uint8_t ldsize_len[] = { 0x48, 0x8b, 0x80 };
                emit(jit.buf, jit.size, ldsize_len, 3);
                emit32(jit.buf, jit.size, strSizeOff); // mov rax, [rax+strSizeOff]
                // NaN-box as int: shl 16; shr 16; or TAG_INT
                emit(jit.buf, jit.size, shl16_len, 4);
                emit(jit.buf, jit.size, shr16_len, 4);
                uint8_t movabs_tag_len[] = { 0x48, 0xb9 };
                emit(jit.buf, jit.size, movabs_tag_len, 2);
                emit64(jit.buf, jit.size, NB_TAG_INT);
                uint8_t orrax_len[] = { 0x48, 0x09, 0xc8 };
                emit(jit.buf, jit.size, orrax_len, 3);
                // Store result
                uint8_t stval_len[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval_len, 3);
                emit32(jit.buf, jit.size, A * VAL_SIZE);
                uint8_t jmp_len[] = { 0xe9 };
                emit(jit.buf, jit.size, jmp_len, 1);
                size_t lenDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // slow path: C helper for non-strings (vectors, tables, etc.)
                patchRel32(jit.buf, lenSlowPatch, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitLength, A, A, &errorExitPatches);
                patchRel32(jit.buf, lenDonePatch, jit.size);
            } else {
                uint8_t a1[] = { 0x48, 0x89, 0xdf }; emit(jit.buf, jit.size, a1, 3);
                uint8_t a2[] = { 0x4c, 0x89, 0xe6 }; emit(jit.buf, jit.size, a2, 3);
                uint8_t a3[] = { 0xba }; emit(jit.buf, jit.size, a3, 1); emit32(jit.buf, jit.size, A);
                uint8_t a4[] = { 0xb9 }; emit(jit.buf, jit.size, a4, 1); emit32(jit.buf, jit.size, B);
                uint8_t movr8[] = { 0x49, 0xb8 }; emit(jit.buf, jit.size, movr8, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(methodStr));
                uint8_t movabs[] = { 0x48, 0xb8 }; emit(jit.buf, jit.size, movabs, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitMcall));
                uint8_t call[] = { 0xff, 0xd0 }; emit(jit.buf, jit.size, call, 2);
                emitErrorCheck(jit.buf, jit.size, errorExitPatches);
            }
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
        case OP_GETCAPTURE: {
            uint8_t load0[] = { 0x48, 0x8b, 0x03 };
            emit(jit.buf, jit.size, load0, 3);
            uint8_t movabs_mask[] = { 0x48, 0xb9 };
            emit(jit.buf, jit.size, movabs_mask, 2);
            emit64(jit.buf, jit.size, 0x0000FFFFFFFFFFFFULL);
            uint8_t and_op[] = { 0x48, 0x21, 0xc8 };
            emit(jit.buf, jit.size, and_op, 3);
            uint8_t load_cap[] = { 0x48, 0x8b, 0x80 };
            emit(jit.buf, jit.size, load_cap, 3);
            emit32(jit.buf, jit.size, static_cast<uint32_t>(capturesOff));
            uint8_t load_elem[] = { 0x48, 0x8b, 0x80 };
            emit(jit.buf, jit.size, load_elem, 3);
            emit32(jit.buf, jit.size, B * VAL_SIZE);
            uint8_t store[] = { 0x48, 0x89, 0x83 };
            emit(jit.buf, jit.size, store, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE);
            break;
        }
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
        case OP_INT:
            emitCallHelper2(jit.buf, jit.size, (void*)jitInt, A, B, &errorExitPatches);
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
        patchRel32(jit.buf, p, lightEntry);
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

    // Error exit: load NaN-boxed null into rax, marker-aware epilogue
    size_t errorExit = jit.size;
    {
        uint8_t movabs[] = { 0x48, 0xb8 };
        emit(jit.buf, jit.size, movabs, 2);
        emit64(jit.buf, jit.size, BblValue::TAG_NULL);
        // test r14d, r14d
        uint8_t test[] = { 0x45, 0x85, 0xf6 };
        emit(jit.buf, jit.size, test, 3);
        uint8_t jnz[] = { 0x75, 0x0c };
        emit(jit.buf, jit.size, jnz, 2);
        uint8_t fullEpi[] = { 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5b, 0xc3 };
        emit(jit.buf, jit.size, fullEpi, sizeof(fullEpi));
        uint8_t lightEpi[] = { 0x48, 0x83, 0xc4, 0x08, 0x41, 0x5e, 0x5b, 0xc3 };
        emit(jit.buf, jit.size, lightEpi, sizeof(lightEpi));
    }
    for (size_t p : errorExitPatches) {
        patchRel32(jit.buf, p, errorExit);
    }

    jitProtect(jit.buf, jit.capacity);
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

BblValue jitCallChecked(BblValue* regs, BblState* state, uint8_t argc) {
    g_jitError = false;
    jitCall(regs, state, 0, argc);
    if (g_jitError) {
        g_jitError = false;
        throw BBL::Error{g_jitErrorMsg};
    }
    return regs[0];
}

void jitFree(JitCode& jit) {
    if (jit.buf) {
        jitFree(jit.buf, jit.capacity);
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

        trace.entries.push_back({inst, curChunk, regBase, false, false,
            static_cast<uint8_t>(regs[decodeA(inst) + regBase].type())});
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
            size_t target = pc - 1 + 1 + off;
            if (target > loopPc) {
                // Jump past loop end — this is the loop exit (not-taken path of the condition).
                // Don't follow; the hot path stays in the loop. Remove the JMP entry.
                trace.entries.pop_back();
            } else {
                pc = target;
            }
        }
        if (op == OP_LTJMP || op == OP_LEJMP || op == OP_GTJMP || op == OP_GEJMP) {
        }
        if (op == OP_JMPFALSE || op == OP_JMPTRUE) {
            int off = decodesBx(inst);
            uint8_t A = decodeA(inst);
            BblValue& cond = regs[A + regBase];
            bool falsy = cond.type() == BBL::Type::Null ||
                         (cond.type() == BBL::Type::Bool && !cond.boolVal()) ||
                         (cond.type() == BBL::Type::Int && cond.intVal() == 0);
            bool taken = (op == OP_JMPFALSE) ? falsy : !falsy;
            trace.entries.back().branchTaken = taken;
            if (taken) pc = pc - 1 + 1 + off;
        }

        if (op == OP_TRYBEGIN || op == OP_TRYEND ||
            op == OP_EXEC || op == OP_EXECFILE || op == OP_STRUCT || op == OP_VECTOR) {
            return trace;
        }
    }
    return trace;
}

static bool traceOpWritesA(uint8_t op) {
    switch (op) {
    case OP_LOADK: case OP_LOADNULL: case OP_LOADBOOL: case OP_LOADINT:
    case OP_ADD: case OP_ADDK: case OP_ADDI: case OP_SUB: case OP_SUBI:
    case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LTE: case OP_GTE:
    case OP_NOT: case OP_MOVE:
    case OP_GETGLOBAL: case OP_ENVGET: case OP_GETCAPTURE:
    case OP_GETFIELD: case OP_GETINDEX: case OP_LENGTH:
    case OP_TABLE: case OP_CLOSURE:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT: case OP_SHL: case OP_SHR:
        return true;
    default: return false;
    }
}

static bool traceOpReadsReg(uint8_t op, uint8_t A, uint8_t B, uint8_t C, uint8_t reg) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LTE: case OP_GTE:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
        return reg == B || reg == C;
    case OP_ADDI: case OP_SUBI: case OP_NOT: case OP_BNOT: case OP_MOVE:
    case OP_LENGTH: case OP_SETGLOBAL: case OP_ENVSET: case OP_SETCAPTURE:
        return reg == A;
    case OP_ADDK: return reg == B;
    case OP_GETFIELD: return reg == B;
    case OP_SETFIELD: return reg == A || reg == B;
    case OP_GETINDEX: return reg == B || reg == C;
    case OP_SETINDEX: return reg == A || reg == B || reg == C;
    case OP_JMPFALSE: case OP_JMPTRUE: return reg == A;
    case OP_LTJMP: case OP_LEJMP: case OP_GTJMP: case OP_GEJMP: return reg == A || reg == B;
    case OP_CALL: {
        uint8_t argc = B;
        if (reg >= A && reg <= A + argc) return true;
        return false;
    }
    case OP_TABLE: {
        uint8_t pairs = B;
        for (int p = 0; p < pairs; p++)
            if (reg == A + 1 + p * 2 || reg == A + 2 + p * 2) return true;
        return false;
    }
    case OP_RETURN: return reg == A;
    default: return false;
    }
}

static void eliminateDeadStores(Trace& trace) {
    std::unordered_map<uint8_t, size_t> lastWrite;
    std::unordered_set<uint8_t> readRegs;

    for (size_t i = 0; i < trace.entries.size(); i++) {
        auto& e = trace.entries[i];
        if (e.eliminated) continue;
        uint8_t op = decodeOP(e.inst);
        uint8_t A = decodeA(e.inst) + e.regBase;
        uint8_t B = decodeB(e.inst) + e.regBase;
        uint8_t C = decodeC(e.inst) + e.regBase;

        for (uint8_t r = 0; r < 255; r++) {
            if (traceOpReadsReg(op, A, B, C, r)) readRegs.insert(r);
        }

        if (traceOpWritesA(op)) {
            auto it = lastWrite.find(A);
            if (it != lastWrite.end() && readRegs.find(A) == readRegs.end()) {
                auto& prevEntry = trace.entries[it->second];
                uint8_t prevOp = decodeOP(prevEntry.inst);
                if (prevOp == OP_LOADINT || prevOp == OP_LOADK ||
                    prevOp == OP_LOADNULL || prevOp == OP_LOADBOOL ||
                    prevOp == OP_MOVE ||
                    prevOp == OP_ADD || prevOp == OP_SUB || prevOp == OP_MUL ||
                    prevOp == OP_ADDI || prevOp == OP_SUBI || prevOp == OP_ADDK) {
                    prevEntry.eliminated = true;
                }
            }
            lastWrite[A] = i;
            readRegs.erase(A);
        }
    }
}

static void sinkAllocations(BblState& state, Trace& trace) {
    struct AllocInfo {
        size_t tableIdx;
        uint8_t reg;
        bool escaped = false;
        std::vector<std::pair<size_t, std::string>> setFieldIndices;
        std::unordered_map<std::string, uint8_t> fieldToReg;
    };

    std::unordered_map<uint8_t, AllocInfo> allocs;
    std::unordered_map<uint8_t, uint8_t> aliases;

    for (size_t i = 0; i < trace.entries.size(); i++) {
        auto& e = trace.entries[i];
        if (e.eliminated) continue;
        uint8_t op = decodeOP(e.inst);
        uint8_t A = decodeA(e.inst) + e.regBase;
        uint8_t B = decodeB(e.inst) + e.regBase;

        if (op == OP_TABLE) {
            uint8_t pairCount = decodeB(e.inst);
            AllocInfo info = {i, A, false, {}, {}};
            if (pairCount > 0) {
                bool canSink = true;
                for (int p = 0; p < pairCount && canSink; p++) {
                    uint8_t keyReg = A + 1 + p * 2;
                    uint8_t valReg = A + 2 + p * 2;
                    // Find the LOADK that loaded the key to get the field name
                    std::string fieldName;
                    for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                        auto& prev = trace.entries[j];
                        if (prev.eliminated) continue;
                        uint8_t prevOp = decodeOP(prev.inst);
                        uint8_t prevA = decodeA(prev.inst) + prev.regBase;
                        if (prevA == keyReg && prevOp == OP_LOADK) {
                            uint16_t kidx = decodeBx(prev.inst);
                            if (prev.chunk->constants[kidx].type() == BBL::Type::String)
                                fieldName = prev.chunk->constants[kidx].stringVal()->data;
                            break;
                        }
                        if (prevA == keyReg) break;
                    }
                    if (fieldName.empty()) { canSink = false; break; }
                    info.setFieldIndices.push_back({i, fieldName});
                    info.fieldToReg[fieldName] = valReg;
                }
                if (canSink) {
                    allocs[A] = std::move(info);
                }
            } else {
                allocs[A] = std::move(info);
            }
        } else if (op == OP_SETFIELD) {
            uint8_t objReg = decodeB(e.inst) + e.regBase;
            uint8_t nameIdx = decodeC(e.inst);
            auto it = allocs.find(objReg);
            if (it != allocs.end() && !it->second.escaped) {
                std::string fieldName = e.chunk->constants[nameIdx].stringVal()->data;
                it->second.setFieldIndices.push_back({i, fieldName});
                it->second.fieldToReg[fieldName] = A;
            }
        } else if (op == OP_GETFIELD) {
            uint8_t objReg = decodeB(e.inst) + e.regBase;
            uint8_t nameIdx = decodeC(e.inst);
            auto it = allocs.find(objReg);
            if (it != allocs.end() && !it->second.escaped) {
                std::string fieldName = e.chunk->constants[nameIdx].stringVal()->data;
                auto fit = it->second.fieldToReg.find(fieldName);
                if (fit != it->second.fieldToReg.end()) {
                    e.eliminated = true;
                    e.inst = encodeABC(OP_MOVE, A - e.regBase, fit->second - e.regBase, 0);
                    e.eliminated = false;
                }
            }
        } else {
            // MOVE copies reference — track alias
            if (op == OP_MOVE) {
                auto it = allocs.find(B);
                if (it != allocs.end() && !it->second.escaped) {
                    aliases[A] = it->second.reg;
                }
            }
            auto resolveAlias = [&](uint8_t r) -> uint8_t {
                auto ait = aliases.find(r);
                return ait != aliases.end() ? ait->second : r;
            };
            for (auto& [reg, info] : allocs) {
                if (info.escaped) continue;
                if (op == OP_SETGLOBAL || op == OP_ENVSET) {
                    if (resolveAlias(A) == reg) info.escaped = true;
                } else if (op == OP_CALL) {
                    uint8_t argc = decodeB(e.inst);
                    for (uint8_t a = A; a <= A + argc; a++)
                        if (resolveAlias(a) == reg) info.escaped = true;
                } else if (op == OP_SETINDEX) {
                    if (resolveAlias(A) == reg || resolveAlias(B) == reg) info.escaped = true;
                } else if (op == OP_RETURN) {
                    if (resolveAlias(A) == reg) info.escaped = true;
                }
            }
        }

        if (traceOpWritesA(op) && op != OP_TABLE) {
            allocs.erase(A);
        }
    }

    for (auto& [reg, info] : allocs) {
        if (info.escaped) continue;
        if (info.setFieldIndices.empty()) continue;

        trace.entries[info.tableIdx].eliminated = true;
        // For TABLE-with-pairs, setFieldIndices all point to the TABLE entry.
        // Also eliminate LOADK entries that loaded the key strings.
        uint8_t pairCount = decodeB(trace.entries[info.tableIdx].inst);
        if (pairCount > 0) {
            uint8_t baseReg = info.reg;
            for (int p = 0; p < pairCount; p++) {
                uint8_t keyReg = baseReg + 1 + p * 2;
                for (int j = static_cast<int>(info.tableIdx) - 1; j >= 0; j--) {
                    auto& prev = trace.entries[j];
                    if (prev.eliminated) continue;
                    uint8_t prevA = decodeA(prev.inst) + prev.regBase;
                    if (prevA == keyReg && decodeOP(prev.inst) == OP_LOADK) {
                        prev.eliminated = true;
                        break;
                    }
                    if (prevA == keyReg) break;
                }
            }
        } else {
            for (auto& [idx, name] : info.setFieldIndices)
                trace.entries[idx].eliminated = true;
        }

        SunkAllocation sunk;
        sunk.destReg = info.reg;
        for (auto& [name, srcReg] : info.fieldToReg)
            sunk.fields.push_back({name, srcReg});
        trace.sunkAllocs.push_back(std::move(sunk));

        for (auto& [aliasReg, origReg] : aliases) {
            if (origReg == info.reg) {
                for (size_t j = info.tableIdx + 1; j < trace.entries.size(); j++) {
                    auto& ej = trace.entries[j];
                    if (ej.eliminated) continue;
                    if (decodeOP(ej.inst) == OP_MOVE &&
                        decodeA(ej.inst) + ej.regBase == aliasReg &&
                        decodeB(ej.inst) + ej.regBase == info.reg) {
                        ej.eliminated = true;
                        break;
                    }
                }
            }
        }
    }
}

void optimizeTrace(BblState& state, Trace& trace) {
    eliminateDeadStores(trace);
    sinkAllocations(state, trace);
    eliminateDeadStores(trace);
}

JitCode compileTrace(BblState& state, Trace& trace) {
    JitCode jit;
    jit.capacity = trace.entries.size() * 96 + 1024;
    jit.buf = static_cast<uint8_t*>(jitAlloc(jit.capacity));
    jit.size = 0;

    BblTable tblDummy;
    size_t tblArrayOff = reinterpret_cast<char*>(&tblDummy.array) - reinterpret_cast<char*>(&tblDummy);
    size_t tblAsizeOff = reinterpret_cast<char*>(&tblDummy.asize) - reinterpret_cast<char*>(&tblDummy);
    size_t tblCountOff = reinterpret_cast<char*>(&tblDummy.count) - reinterpret_cast<char*>(&tblDummy);

    emitPrologue(jit.buf, jit.size);

    size_t loopStart = jit.size;
    std::vector<size_t> errorExitPatches;

    for (size_t i = 0; i < trace.entries.size(); i++) {
        auto& entry = trace.entries[i];
        if (entry.eliminated) continue;
        uint32_t inst = entry.inst;
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst) + entry.regBase;
        uint8_t B = decodeB(inst) + entry.regBase;
        uint8_t C = decodeC(inst) + entry.regBase;
        int sBx = decodesBx(inst);
        uint16_t Bx = decodeBx(inst);

        switch (op) {
        case OP_ADD: emitAdd(jit.buf, jit.size, A, B, C); break;
        case OP_SUB: emitSub(jit.buf, jit.size, A, B, C); break;
        case OP_MUL: emitMul(jit.buf, jit.size, A, B, C); break;
        case OP_ADDI: emitAddi(jit.buf, jit.size, A, sBx); break;
        case OP_SUBI: emitSubi(jit.buf, jit.size, A, sBx); break;
        case OP_MOVE: emitMove(jit.buf, jit.size, A, B); break;
        case OP_LOADINT: emitLoadInt(jit.buf, jit.size, A, sBx); break;
        case OP_DIV:
            emitCallHelper2(jit.buf, jit.size, (void*)jitArith, A, static_cast<uint32_t>((3 << 16) | (B << 8) | C), &errorExitPatches);
            break;
        case OP_LOADNULL: emitLoadNull(jit.buf, jit.size, A); break;
        case OP_LOADK: emitLoadK(jit.buf, jit.size, A, &entry.chunk->constants[Bx]); break;
        case OP_LOADBOOL: {
            uint64_t bits = (static_cast<uint64_t>(0xFFF9) << 48) | (B ? 1ULL : 0ULL);
            uint8_t movabs[] = { 0x48, 0xb8 };
            emit(jit.buf, jit.size, movabs, 2);
            emit64(jit.buf, jit.size, bits);
            uint8_t store[] = { 0x48, 0x89, 0x83 };
            emit(jit.buf, jit.size, store, 3);
            emit32(jit.buf, jit.size, A * VAL_SIZE);
            break;
        }
        case OP_ADDK: {
            int origA = decodeA(inst) + entry.regBase;
            int origB = decodeB(inst) + entry.regBase;
            int origC = decodeC(inst);
            if (origC >= static_cast<int>(entry.chunk->constants.size()) ||
                entry.chunk->constants[origC].type() != BBL::Type::Int) {
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
            size_t p = emitCompareJmp(jit.buf, jit.size, A, B, 0x8d); // jge = exit when NOT less
            trace.snapshots.push_back({entry.chunk,
                static_cast<size_t>(&entry.chunk->code[0] - entry.chunk->code.data()) + i, entry.regBase});
            errorExitPatches.push_back(p);
            break;
        }
        case OP_LEJMP: {
            size_t p = emitCompareJmp(jit.buf, jit.size, A, B, 0x8f); // jg = exit when NOT le
            trace.snapshots.push_back({entry.chunk, i, entry.regBase});
            errorExitPatches.push_back(p);
            break;
        }
        case OP_GTJMP: {
            size_t p = emitCompareJmp(jit.buf, jit.size, A, B, 0x8e); // jle = exit when NOT gt
            trace.snapshots.push_back({entry.chunk, i, entry.regBase});
            errorExitPatches.push_back(p);
            break;
        }
        case OP_GEJMP: {
            size_t p = emitCompareJmp(jit.buf, jit.size, A, B, 0x8c); // jl = exit when NOT ge
            trace.snapshots.push_back({entry.chunk, i, entry.regBase});
            errorExitPatches.push_back(p);
            break;
        }
        case OP_JMP:
            break;

        case OP_JMPFALSE: case OP_JMPTRUE: {
            uint8_t origA = decodeA(inst) + entry.regBase;
            bool wantFalsy = (op == OP_JMPFALSE && !entry.branchTaken) ||
                             (op == OP_JMPTRUE && entry.branchTaken);
            if (wantFalsy) {
                size_t p = emitJmpFalse(jit.buf, jit.size, origA);
                trace.snapshots.push_back({entry.chunk, i, entry.regBase});
                errorExitPatches.push_back(p);
            } else {
                // Guard: exit if truthy (inverted). emitJmpFalse jumps if falsy.
                // Emit jmpFalse to skip over the side exit.
                size_t p = emitJmpFalse(jit.buf, jit.size, origA);
                // If NOT falsy (truthy), we fall through to the side exit jump
                emit8(jit.buf, jit.size, 0xe9);
                trace.snapshots.push_back({entry.chunk, i, entry.regBase});
                errorExitPatches.push_back(jit.size);
                emit32(jit.buf, jit.size, 0);
                // Patch jmpFalse to skip over the side exit (to here)
                patchRel32(jit.buf, p, jit.size);
            }
            break;
        }

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(entry.chunk->constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetGlobal, symId, A, &errorExitPatches);
            break;
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(entry.chunk->constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetGlobal, symId, A, &errorExitPatches);
            break;
        }
        case OP_ENVGET: {
            uint32_t symId = static_cast<uint32_t>(entry.chunk->constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitEnvGet, symId, A, &errorExitPatches);
            break;
        }
        case OP_ENVSET: {
            uint32_t symId = static_cast<uint32_t>(entry.chunk->constants[Bx].intVal());
            emitCallHelper2(jit.buf, jit.size, (void*)jitEnvSet, symId, A, &errorExitPatches);
            break;
        }

        case OP_TABLE: {
            uint8_t origA = decodeA(inst) + entry.regBase;
            uint8_t pairCount = decodeB(inst);
            emitCallHelper2(jit.buf, jit.size, (void*)jitTable, origA, pairCount, &errorExitPatches);
            break;
        }
        case OP_GETFIELD: {
            uint8_t origA = decodeA(inst) + entry.regBase;
            uint32_t packed = static_cast<uint32_t>(((decodeB(inst) + entry.regBase) << 8) | decodeC(inst));
            // Set r13 to entry.chunk for constant lookup
            uint8_t movabs_r13[] = { 0x49, 0xbd };
            emit(jit.buf, jit.size, movabs_r13, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(entry.chunk));
            emitCallHelper3(jit.buf, jit.size, (void*)jitGetField, 0, origA, packed, &errorExitPatches);
            break;
        }
        case OP_SETFIELD: {
            uint8_t valReg = decodeA(inst) + entry.regBase;
            uint32_t packed = static_cast<uint32_t>(((decodeB(inst) + entry.regBase) << 8) | decodeC(inst));
            uint8_t movabs_r13[] = { 0x49, 0xbd };
            emit(jit.buf, jit.size, movabs_r13, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(entry.chunk));
            emitCallHelper3(jit.buf, jit.size, (void*)jitSetField, 0, valReg, packed, &errorExitPatches);
            break;
        }
        case OP_GETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetIndex,
                static_cast<uint32_t>((A << 16) | (B << 8) | C), 0, &errorExitPatches);
            break;
        case OP_SETINDEX:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetIndex,
                static_cast<uint32_t>((A << 16) | (B << 8) | C), 0, &errorExitPatches);
            break;
        case OP_LENGTH:
            emitCallHelper2(jit.buf, jit.size, (void*)jitLength, A, B, &errorExitPatches);
            break;
        case OP_MCALL: {
            uint8_t origA = decodeA(inst) + entry.regBase;
            uint8_t origB = decodeB(inst);
            uint8_t origC = decodeC(inst);
            BblString* methodStr = entry.chunk->constants[origC].stringVal();

            if (methodStr == state.m.get) {
                // Inline array get: extract table ptr, check int key, bounds, load
                uint8_t ld[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ld, 3);
                emit32(jit.buf, jit.size, origA * VAL_SIZE); // mov rax, [rbx+A*8] (table)
                uint8_t shl16[] = { 0x48, 0xc1, 0xe0, 0x10 };
                emit(jit.buf, jit.size, shl16, 4);
                uint8_t shr16[] = { 0x48, 0xc1, 0xe8, 0x10 };
                emit(jit.buf, jit.size, shr16, 4); // rax = table ptr
                uint8_t movrdi[] = { 0x48, 0x89, 0xc7 };
                emit(jit.buf, jit.size, movrdi, 3); // mov rdi, rax
                // Load key
                uint8_t ldkey[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey, 3);
                emit32(jit.buf, jit.size, (origA + 1) * VAL_SIZE); // mov rsi, key
                // Check TAG_INT
                uint8_t movrax_rsi[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi, 3);
                uint8_t shrrax48[] = { 0x48, 0xc1, 0xe8, 0x30 };
                emit(jit.buf, jit.size, shrrax48, 4);
                uint8_t cmpax[] = { 0x66, 0x3d };
                emit(jit.buf, jit.size, cmpax, 2);
                emit16(jit.buf, jit.size, 0xFFFA);
                uint8_t jne_g[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_g, 2);
                size_t gSlowPatch1 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Extract int
                uint8_t shlrsi[] = { 0x48, 0xc1, 0xe6, 0x10 };
                emit(jit.buf, jit.size, shlrsi, 4);
                uint8_t sarrsi[] = { 0x48, 0xc1, 0xfe, 0x10 };
                emit(jit.buf, jit.size, sarrsi, 4);
                // Bounds: test rsi, rsi; js slow
                uint8_t testrsi[] = { 0x48, 0x85, 0xf6 };
                emit(jit.buf, jit.size, testrsi, 3);
                uint8_t js_g[] = { 0x0f, 0x88 };
                emit(jit.buf, jit.size, js_g, 2);
                size_t gSlowPatch2 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // cmp esi, [rdi+tblAsizeOff]
                uint8_t cmpesi[] = { 0x3b, 0xb7 };
                emit(jit.buf, jit.size, cmpesi, 2);
                emit32(jit.buf, jit.size, tblAsizeOff);
                uint8_t jae_g[] = { 0x0f, 0x83 };
                emit(jit.buf, jit.size, jae_g, 2);
                size_t gSlowPatch3 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Load array[k]
                uint8_t ldArr[] = { 0x48, 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldArr, 3);
                emit32(jit.buf, jit.size, tblArrayOff);
                uint8_t ldVal[] = { 0x48, 0x8b, 0x04, 0xf1 };
                emit(jit.buf, jit.size, ldVal, 4); // mov rax, [rcx+rsi*8]
                uint8_t testrax[] = { 0x48, 0x85, 0xc0 };
                emit(jit.buf, jit.size, testrax, 3);
                uint8_t jz_g[] = { 0x0f, 0x84 };
                emit(jit.buf, jit.size, jz_g, 2);
                size_t gSlowPatch4 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Store result
                uint8_t stval[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stval, 3);
                emit32(jit.buf, jit.size, origA * VAL_SIZE);
                uint8_t jmpDone[] = { 0xe9 };
                emit(jit.buf, jit.size, jmpDone, 1);
                size_t gDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Slow path
                patchRel32(jit.buf, gSlowPatch1, jit.size);
                patchRel32(jit.buf, gSlowPatch2, jit.size);
                patchRel32(jit.buf, gSlowPatch3, jit.size);
                patchRel32(jit.buf, gSlowPatch4, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitTableGet, origA, origB, &errorExitPatches);
                patchRel32(jit.buf, gDonePatch, jit.size);

            } else if (methodStr == state.m.set) {
                // Inline array set: extract table ptr, check int key, bounds, store
                uint8_t ld[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ld, 3);
                emit32(jit.buf, jit.size, origA * VAL_SIZE);
                uint8_t shl16[] = { 0x48, 0xc1, 0xe0, 0x10 };
                emit(jit.buf, jit.size, shl16, 4);
                uint8_t shr16[] = { 0x48, 0xc1, 0xe8, 0x10 };
                emit(jit.buf, jit.size, shr16, 4);
                uint8_t movrdi[] = { 0x48, 0x89, 0xc7 };
                emit(jit.buf, jit.size, movrdi, 3);
                // Load + check key
                uint8_t ldkey[] = { 0x48, 0x8b, 0xb3 };
                emit(jit.buf, jit.size, ldkey, 3);
                emit32(jit.buf, jit.size, (origA + 1) * VAL_SIZE);
                uint8_t movrax_rsi[] = { 0x48, 0x89, 0xf0 };
                emit(jit.buf, jit.size, movrax_rsi, 3);
                uint8_t shrrax48[] = { 0x48, 0xc1, 0xe8, 0x30 };
                emit(jit.buf, jit.size, shrrax48, 4);
                uint8_t cmpax[] = { 0x66, 0x3d };
                emit(jit.buf, jit.size, cmpax, 2);
                emit16(jit.buf, jit.size, 0xFFFA);
                uint8_t jne_s[] = { 0x0f, 0x85 };
                emit(jit.buf, jit.size, jne_s, 2);
                size_t sSlowPatch1 = jit.size;
                emit32(jit.buf, jit.size, 0);
                uint8_t shlrsi[] = { 0x48, 0xc1, 0xe6, 0x10 };
                emit(jit.buf, jit.size, shlrsi, 4);
                uint8_t sarrsi[] = { 0x48, 0xc1, 0xfe, 0x10 };
                emit(jit.buf, jit.size, sarrsi, 4);
                uint8_t testrsi[] = { 0x48, 0x85, 0xf6 };
                emit(jit.buf, jit.size, testrsi, 3);
                uint8_t js_s[] = { 0x0f, 0x88 };
                emit(jit.buf, jit.size, js_s, 2);
                size_t sSlowPatch2 = jit.size;
                emit32(jit.buf, jit.size, 0);
                uint8_t cmpesi[] = { 0x3b, 0xb7 };
                emit(jit.buf, jit.size, cmpesi, 2);
                emit32(jit.buf, jit.size, tblAsizeOff);
                uint8_t jae_s[] = { 0x0f, 0x83 };
                emit(jit.buf, jit.size, jae_s, 2);
                size_t sSlowPatch3 = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Load array ptr
                uint8_t ldArr[] = { 0x48, 0x8b, 0x8f };
                emit(jit.buf, jit.size, ldArr, 3);
                emit32(jit.buf, jit.size, tblArrayOff);
                // Check empty → je newkey
                uint8_t cmpSlot[] = { 0x48, 0x83, 0x3c, 0xf1, 0x00 };
                emit(jit.buf, jit.size, cmpSlot, 5);
                uint8_t je_nk[] = { 0x74 };
                emit(jit.buf, jit.size, je_nk, 1);
                size_t nkPatch = jit.size;
                emit8(jit.buf, jit.size, 0);
                // Existing: store value
                uint8_t ldnv[] = { 0x48, 0x8b, 0x83 };
                emit(jit.buf, jit.size, ldnv, 3);
                emit32(jit.buf, jit.size, (origA + 2) * VAL_SIZE);
                uint8_t stArr[] = { 0x48, 0x89, 0x04, 0xf1 };
                emit(jit.buf, jit.size, stArr, 4);
                emitWriteBarrier(jit.buf, jit.size);
                uint8_t jmpEx[] = { 0xe9 };
                emit(jit.buf, jit.size, jmpEx, 1);
                size_t exPatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // New key: store + inc count
                jit.buf[nkPatch] = static_cast<uint8_t>(jit.size - nkPatch - 1);
                emit(jit.buf, jit.size, ldnv, 3);
                emit32(jit.buf, jit.size, (origA + 2) * VAL_SIZE);
                emit(jit.buf, jit.size, stArr, 4);
                emitWriteBarrier(jit.buf, jit.size);
                uint8_t incCnt[] = { 0xff, 0x87 };
                emit(jit.buf, jit.size, incCnt, 2);
                emit32(jit.buf, jit.size, tblCountOff);
                // Done
                patchRel32(jit.buf, exPatch, jit.size);
                // Store null result
                uint8_t movNull[] = { 0x48, 0xb8 };
                emit(jit.buf, jit.size, movNull, 2);
                emit64(jit.buf, jit.size, NB_TAG_NULL);
                uint8_t stRes[] = { 0x48, 0x89, 0x83 };
                emit(jit.buf, jit.size, stRes, 3);
                emit32(jit.buf, jit.size, origA * VAL_SIZE);
                uint8_t jmpDone[] = { 0xe9 };
                emit(jit.buf, jit.size, jmpDone, 1);
                size_t sDonePatch = jit.size;
                emit32(jit.buf, jit.size, 0);
                // Slow path
                patchRel32(jit.buf, sSlowPatch1, jit.size);
                patchRel32(jit.buf, sSlowPatch2, jit.size);
                patchRel32(jit.buf, sSlowPatch3, jit.size);
                emitCallHelper2(jit.buf, jit.size, (void*)jitTableSet, origA, origB, &errorExitPatches);
                patchRel32(jit.buf, sDonePatch, jit.size);

            } else if (methodStr == state.m.length) {
                emitCallHelper2(jit.buf, jit.size, (void*)jitLength, origA, origA, &errorExitPatches);
            } else {
                uint8_t a1[] = { 0x48, 0x89, 0xdf }; emit(jit.buf, jit.size, a1, 3);
                uint8_t a2[] = { 0x4c, 0x89, 0xe6 }; emit(jit.buf, jit.size, a2, 3);
                uint8_t a3[] = { 0xba }; emit(jit.buf, jit.size, a3, 1); emit32(jit.buf, jit.size, origA);
                uint8_t a4[] = { 0xb9 }; emit(jit.buf, jit.size, a4, 1); emit32(jit.buf, jit.size, origB);
                uint8_t movr8[] = { 0x49, 0xb8 }; emit(jit.buf, jit.size, movr8, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(methodStr));
                uint8_t movabs[] = { 0x48, 0xb8 }; emit(jit.buf, jit.size, movabs, 2);
                emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>((void*)jitMcall));
                uint8_t call[] = { 0xff, 0xd0 }; emit(jit.buf, jit.size, call, 2);
                emitErrorCheck(jit.buf, jit.size, errorExitPatches);
            }
            break;
        }

        case OP_EQ: emitCmp(jit.buf, jit.size, A, B, C, 0x94); break;
        case OP_NEQ: emitCmp(jit.buf, jit.size, A, B, C, 0x95); break;
        case OP_LT: emitCmp(jit.buf, jit.size, A, B, C, 0x9c); break;
        case OP_GT: emitCmp(jit.buf, jit.size, A, B, C, 0x9f); break;
        case OP_LTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9e); break;
        case OP_GTE: emitCmp(jit.buf, jit.size, A, B, C, 0x9d); break;
        case OP_NOT:
            emitCallHelper2(jit.buf, jit.size, (void*)jitNot, A, B, &errorExitPatches);
            break;
        case OP_AND: case OP_OR:
            break;

        case OP_CLOSURE: {
            uint8_t origA = decodeA(inst) + entry.regBase;
            uint16_t origBx = decodeBx(inst);
            uint8_t movabs_r13[] = { 0x49, 0xbd };
            emit(jit.buf, jit.size, movabs_r13, 2);
            emit64(jit.buf, jit.size, reinterpret_cast<uint64_t>(entry.chunk));
            emitCallHelper3(jit.buf, jit.size, (void*)jitClosure, 0, origA, origBx, &errorExitPatches);
            break;
        }
        case OP_GETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitGetCapture, A, B, &errorExitPatches);
            break;
        case OP_SETCAPTURE:
            emitCallHelper2(jit.buf, jit.size, (void*)jitSetCapture, A, B, &errorExitPatches);
            break;

        case OP_CALL:
            break;
        case OP_RETURN: {
            uint8_t retReg = decodeA(inst) + entry.regBase;
            for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                if (decodeOP(trace.entries[j].inst) == OP_CALL &&
                    trace.entries[j].regBase < entry.regBase) {
                    uint8_t callA = decodeA(trace.entries[j].inst) + trace.entries[j].regBase;
                    if (retReg != callA)
                        emitMove(jit.buf, jit.size, callA, retReg);
                    break;
                }
            }
            break;
        }

        default:
            break;
        }
    }

    // Loop back-edge
    {
        emit8(jit.buf, jit.size, 0xe9);
        int32_t rel = static_cast<int32_t>(loopStart - (jit.size + 4));
        emit32(jit.buf, jit.size, static_cast<uint32_t>(rel));
    }

    // Side exit stubs — each returns its exit index
    size_t exitStart = jit.size;
    for (size_t i = 0; i < errorExitPatches.size(); i++) {
        size_t stubAddr = jit.size;
        patchRel32(jit.buf, errorExitPatches[i], stubAddr);
        // mov eax, exitIndex+1 (0 = completed, 1+ = side exit)
        uint8_t mov_eax[] = { 0xb8 };
        emit(jit.buf, jit.size, mov_eax, 1);
        emit32(jit.buf, jit.size, static_cast<uint32_t>(i + 1));
        emitEpilogue(jit.buf, jit.size);
    }

    // Normal completion: return 0
    size_t completionExit = jit.size;
    {
        uint8_t xor_rax[] = { 0x48, 0x31, 0xc0 };
        emit(jit.buf, jit.size, xor_rax, 3);
        emitEpilogue(jit.buf, jit.size);
    }

    // g_jitError exits also go to side exit 0 (abort)
    // The errorExitPatches from emitErrorCheck go here too — they check g_jitError
    // which is set by C helpers. Map them to a generic abort.
    // Actually, emitErrorCheck patches were already added to errorExitPatches
    // by the emitCallHelper2 calls. They'll get valid stubs above.

    jitProtect(jit.buf, jit.capacity);
    return jit;
}

TraceResult executeTrace(JitCode& jit, BblValue* regs, BblState* state) {
    if (!jit.buf) return {false, 0};
    typedef int64_t (*TraceFn)(BblValue*, BblState*, void*);
    TraceFn fn = reinterpret_cast<TraceFn>(jit.buf);
    int64_t result = fn(regs, state, nullptr);
    return {result == 0, static_cast<size_t>(result)};
}
