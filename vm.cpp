#include "vm.h"
#include "bbl.h"
#include "compiler.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

size_t Chunk::addConstant(const BblValue& val) {
    constants.push_back(val);
    return constants.size() - 1;
}

static bool isFalsy(const BblValue& v) {
    if (v.type == BBL::Type::Null) return true;
    if (v.type == BBL::Type::Bool) return !v.boolVal;
    if (v.type == BBL::Type::Int) return v.intVal == 0;
    return false;
}

static double toFloat(const BblValue& v) {
    if (v.type == BBL::Type::Int) return static_cast<double>(v.intVal);
    if (v.type == BBL::Type::Float) return v.floatVal;
    throw BBL::Error{"expected number"};
}

static std::string valToStr(BblState& state, const BblValue& v) {
    switch (v.type) {
        case BBL::Type::Null: return "null";
        case BBL::Type::Bool: return v.boolVal ? "true" : "false";
        case BBL::Type::Int: return std::to_string(v.intVal);
        case BBL::Type::Float: { char b[64]; snprintf(b, 64, "%g", v.floatVal); return b; }
        case BBL::Type::String: return v.stringVal->data;
        default: return "<" + std::string(typeName(v.type)) + ">";
    }
}

static bool callValue(BblState& state, CallFrame*& frame, uint8_t base, uint8_t argc, uint8_t destInCaller);

InterpretResult vmExecute(BblState& state, Chunk& chunk) {
    state.vm->reset();

    CallFrame* frame = &state.vm->frames[0];
    frame->chunk = &chunk;
    frame->ip = chunk.code.data();
    frame->regs = state.vm->stack.data();
    frame->closure = nullptr;
    frame->numRegs = chunk.numRegs;
    state.vm->frameCount = 1;
    state.vm->stackTop = state.vm->stack.data() + chunk.numRegs;

    #define R(i) frame->regs[i]
    #define K(i) frame->chunk->constants[i]

    for (;;) {
      try {
        uint32_t inst = *frame->ip++;
        uint8_t op = decodeOP(inst);
        uint8_t A = decodeA(inst);
        uint8_t B = decodeB(inst);
        uint8_t C = decodeC(inst);
        uint16_t Bx = decodeBx(inst);
        int sBx = decodesBx(inst);

        switch (op) {

        case OP_LOADK:    R(A) = K(Bx); break;
        case OP_LOADNULL: R(A) = BblValue::makeNull(); break;
        case OP_LOADBOOL: R(A) = BblValue::makeBool(B != 0); break;
        case OP_LOADINT:  R(A) = BblValue::makeInt(static_cast<int64_t>(sBx)); break;

        case OP_ADD: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int)
                R(A) = BblValue::makeInt(rb.intVal + rc.intVal);
            else if (rb.type == BBL::Type::String)
                R(A) = BblValue::makeString(state.intern(rb.stringVal->data + valToStr(state, rc)));
            else
                R(A) = BblValue::makeFloat(toFloat(rb) + toFloat(rc));
            break;
        }
        case OP_ADDK: {
            BblValue& rb = R(B); BblValue& kc = K(C);
            if (rb.type == BBL::Type::Int && kc.type == BBL::Type::Int)
                R(A) = BblValue::makeInt(rb.intVal + kc.intVal);
            else if (rb.type == BBL::Type::String)
                R(A) = BblValue::makeString(state.intern(rb.stringVal->data + valToStr(state, kc)));
            else
                R(A) = BblValue::makeFloat(toFloat(rb) + toFloat(kc));
            break;
        }
        case OP_SUB: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeInt(rb.intVal - rc.intVal);
            else R(A) = BblValue::makeFloat(toFloat(rb) - toFloat(rc));
            break;
        }
        case OP_MUL: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeInt(rb.intVal * rc.intVal);
            else R(A) = BblValue::makeFloat(toFloat(rb) * toFloat(rc));
            break;
        }
        case OP_DIV: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) {
                if (rc.intVal == 0) throw BBL::Error{"division by zero"};
                R(A) = BblValue::makeInt(rb.intVal / rc.intVal);
            } else {
                double d = toFloat(rc); if (d == 0.0) throw BBL::Error{"division by zero"};
                R(A) = BblValue::makeFloat(toFloat(rb) / d);
            }
            break;
        }
        case OP_MOD: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rc.intVal == 0) throw BBL::Error{"modulo by zero"};
            R(A) = BblValue::makeInt(rb.intVal % rc.intVal);
            break;
        }

        case OP_BAND: R(A) = BblValue::makeInt(R(B).intVal & R(C).intVal); break;
        case OP_BOR:  R(A) = BblValue::makeInt(R(B).intVal | R(C).intVal); break;
        case OP_BXOR: R(A) = BblValue::makeInt(R(B).intVal ^ R(C).intVal); break;
        case OP_BNOT: R(A) = BblValue::makeInt(~R(B).intVal); break;
        case OP_SHL:  R(A) = BblValue::makeInt(R(B).intVal << R(C).intVal); break;
        case OP_SHR:  R(A) = BblValue::makeInt(R(B).intVal >> R(C).intVal); break;

        case OP_EQ:  R(A) = BblValue::makeBool(R(B) == R(C)); break;
        case OP_NEQ: R(A) = BblValue::makeBool(R(B) != R(C)); break;
        case OP_LT: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal < rc.intVal);
            else R(A) = BblValue::makeBool(toFloat(rb) < toFloat(rc));
            break;
        }
        case OP_GT: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal > rc.intVal);
            else R(A) = BblValue::makeBool(toFloat(rb) > toFloat(rc));
            break;
        }
        case OP_LTE: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal <= rc.intVal);
            else R(A) = BblValue::makeBool(toFloat(rb) <= toFloat(rc));
            break;
        }
        case OP_GTE: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type == BBL::Type::Int && rc.type == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal >= rc.intVal);
            else R(A) = BblValue::makeBool(toFloat(rb) >= toFloat(rc));
            break;
        }

        case OP_NOT: R(A) = BblValue::makeBool(isFalsy(R(B))); break;
        case OP_MOVE: R(A) = R(B); break;

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal);
            auto it = state.vm->globals.find(symId);
            if (it != state.vm->globals.end()) { R(A) = it->second; break; }
            auto* val = state.rootScope.lookup(symId);
            if (val) { R(A) = *val; break; }
            for (auto& [name, id] : state.symbolIds)
                if (id == symId) throw BBL::Error{"undefined variable: " + name};
            throw BBL::Error{"undefined variable"};
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal);
            state.vm->globals[symId] = R(A);
            state.rootScope.def(symId, R(A));
            break;
        }
        case OP_GETCAPTURE:
            if (!frame->closure) throw BBL::Error{"no closure"};
            R(A) = frame->closure->captures[B];
            break;
        case OP_SETCAPTURE:
            if (!frame->closure) throw BBL::Error{"no closure"};
            frame->closure->captures[B] = R(A);
            break;

        case OP_JMP: frame->ip += sBx; break;
        case OP_JMPFALSE: if (isFalsy(R(A))) frame->ip += sBx; break;
        case OP_JMPTRUE:  if (!isFalsy(R(A))) frame->ip += sBx; break;
        case OP_LOOP:
            frame->ip -= sBx;
            if (state.allocCount >= state.gcThreshold) state.gc();
            if (state.maxSteps && ++state.stepCount > state.maxSteps)
                throw BBL::Error{"step limit exceeded"};
            if (state.terminated.load(std::memory_order_relaxed))
                throw BblTerminated{};
            break;

        case OP_AND: if (isFalsy(R(A))) frame->ip += sBx; break;
        case OP_OR:  if (!isFalsy(R(A))) frame->ip += sBx; break;

        case OP_CLOSURE: {
            BblClosure* proto = K(Bx).closureVal;
            BblClosure* closure = new BblClosure();
            closure->chunk = proto->chunk;
            closure->arity = proto->arity;
            closure->name = proto->name;
            closure->captureDescs = proto->captureDescs;
            closure->captures.resize(proto->captureDescs.size());
            state.allocatedClosures.push_back(closure);

            for (size_t i = 0; i < proto->captureDescs.size(); i++) {
                auto& desc = proto->captureDescs[i];
                if (desc.srcType == 0)
                    closure->captures[i] = R(desc.srcIdx);
                else if (frame->closure)
                    closure->captures[i] = frame->closure->captures[desc.srcIdx];
                else
                    closure->captures[i] = BblValue::makeNull();
            }

            R(A) = BblValue::makeClosure(closure);
            break;
        }

        case OP_CALL: {
            if (!callValue(state, frame, A, B, A))
                throw BBL::Error{"not callable"};
            if (state.maxSteps && ++state.stepCount > state.maxSteps)
                throw BBL::Error{"step limit exceeded"};
            break;
        }

        case OP_TAILCALL: {
            BblValue callee = R(A);
            if (callee.type != BBL::Type::Fn || callee.isCFn || !callee.isClosure)
                throw BBL::Error{"tail call target must be a bytecode function"};
            BblClosure* closure = callee.closureVal;
            if (closure->arity != B)
                throw BBL::Error{"wrong number of arguments"};
            // Copy args to current frame
            R(0) = callee;
            for (int i = 0; i < B; i++)
                R(1 + i) = frame->regs[A + 1 + i];
            frame->chunk = &closure->chunk;
            frame->ip = closure->chunk.code.data();
            frame->closure = closure;
            frame->numRegs = closure->chunk.numRegs;
            state.vm->stackTop = frame->regs + closure->chunk.numRegs;
            break;
        }

        case OP_RETURN: {
            BblValue result = R(A);
            uint8_t returnDest = static_cast<uint8_t>(frame->line);
            state.vm->frameCount--;
            if (state.vm->frameCount == 0) {
                state.vm->stackTop = state.vm->stack.data();
                state.vm->stack[0] = result;
                goto done;
            }
            frame = &state.vm->frames[state.vm->frameCount - 1];
            state.vm->stackTop = frame->regs + frame->numRegs;
            R(returnDest) = result;
            break;
        }

        case OP_VECTOR: {
            // A=dest, B=argc, C=typeNameConstIdx
            std::string elemType = K(C).stringVal->data;
            BBL::Type elemTypeTag = BBL::Type::Null;
            size_t elemSize = 0;
            auto dit = state.structDescs.find(elemType);
            if (dit != state.structDescs.end()) { elemTypeTag = BBL::Type::Struct; elemSize = dit->second.totalSize; }
            else if (elemType == "int" || elemType == "int64") { elemTypeTag = BBL::Type::Int; elemSize = 8; }
            else if (elemType == "float" || elemType == "float64") { elemTypeTag = BBL::Type::Float; elemSize = 8; }
            else if (elemType == "float32") { elemTypeTag = BBL::Type::Float; elemSize = 4; }
            else if (elemType == "int32") { elemTypeTag = BBL::Type::Int; elemSize = 4; }
            else if (elemType == "int16") { elemTypeTag = BBL::Type::Int; elemSize = 2; }
            else if (elemType == "int8" || elemType == "uint8") { elemTypeTag = BBL::Type::Int; elemSize = 1; }
            else throw BBL::Error{"unknown vector element type: " + elemType};
            BblVec* vec = state.allocVector(elemType, elemTypeTag, elemSize);
            for (int i = 0; i < B; i++) state.packValue(vec, R(A + 1 + i));
            R(A) = BblValue::makeVector(vec);
            break;
        }

        case OP_TABLE: {
            BblTable* tbl = state.allocTable();
            for (int i = 0; i < B; i++)
                tbl->set(R(A + 1 + i*2), R(A + 2 + i*2));
            R(A) = BblValue::makeTable(tbl);
            break;
        }

        case OP_STRUCT: {
            // A=dest, B=argc, C=typeNameConstIdx
            std::string tname = K(C).stringVal->data;
            auto dit = state.structDescs.find(tname);
            if (dit == state.structDescs.end()) throw BBL::Error{"unknown struct type: " + tname};
            std::vector<BblValue> args(B);
            for (int i = 0; i < B; i++) args[i] = R(A + 1 + i);
            int line = frame->chunk->lines[frame->ip - frame->chunk->code.data() - 1];
            R(A) = state.constructStruct(&dit->second, args, line);
            break;
        }

        case OP_BINARY: {
            BblValue& arg = R(B);
            if (arg.type == BBL::Type::Vector) R(A) = BblValue::makeBinary(state.allocBinary(arg.vectorVal->data));
            else if (arg.type == BBL::Type::Struct) R(A) = BblValue::makeBinary(state.allocBinary(arg.structVal->data));
            else if (arg.type == BBL::Type::Int) {
                if (arg.intVal < 0) throw BBL::Error{"binary: size must be non-negative"};
                R(A) = BblValue::makeBinary(state.allocBinary(std::vector<uint8_t>(static_cast<size_t>(arg.intVal), 0)));
            } else throw BBL::Error{"binary: invalid argument type"};
            break;
        }

        case OP_GETFIELD: {
            // A=dest, B=obj, C=fieldNameConstIdx
            BblValue& obj = R(B);
            std::string fieldName = K(C).stringVal->data;
            if (obj.type == BBL::Type::Struct) {
                for (auto& fd : obj.structVal->desc->fields) {
                    if (fd.name == fieldName) { R(A) = state.readField(obj.structVal, fd); goto field_ok; }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type == BBL::Type::Table) {
                auto res = obj.tableVal->get(BblValue::makeString(state.intern(fieldName)));
                R(A) = res.value_or(BblValue::makeNull());
            } else throw BBL::Error{"cannot access field on " + std::string(::typeName(obj.type))};
            field_ok: break;
        }

        case OP_SETFIELD: {
            // A=val, B=obj, C=fieldNameConstIdx
            BblValue& obj = R(B);
            std::string fieldName = K(C).stringVal->data;
            if (obj.type == BBL::Type::Struct) {
                for (auto& fd : obj.structVal->desc->fields) {
                    if (fd.name == fieldName) { state.writeField(obj.structVal, fd, R(A)); goto setf_ok; }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type == BBL::Type::Table) {
                obj.tableVal->set(BblValue::makeString(state.intern(fieldName)), R(A));
            } else throw BBL::Error{"cannot set field"};
            setf_ok: break;
        }

        case OP_GETINDEX: {
            BblValue& obj = R(B); BblValue& idx = R(C);
            if (obj.type == BBL::Type::Vector) {
                if (idx.type != BBL::Type::Int) throw BBL::Error{"vector index must be int"};
                R(A) = state.readVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal));
            } else if (obj.type == BBL::Type::Table) {
                R(A) = obj.tableVal->get(idx).value_or(BblValue::makeNull());
            } else if (obj.type == BBL::Type::Binary) {
                size_t i = static_cast<size_t>(idx.intVal);
                if (i >= obj.binaryVal->length()) throw BBL::Error{"binary index out of range"};
                R(A) = BblValue::makeInt(obj.binaryVal->data[i]);
            } else throw BBL::Error{"cannot index " + std::string(::typeName(obj.type))};
            break;
        }

        case OP_SETINDEX: {
            // A=val, B=obj, C=idx
            BblValue& obj = R(B); BblValue& idx = R(C);
            if (obj.type == BBL::Type::Vector) state.writeVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal), R(A));
            else if (obj.type == BBL::Type::Table) obj.tableVal->set(idx, R(A));
            else throw BBL::Error{"cannot set index"};
            break;
        }

        case OP_MCALL: {
            // A=receiver(+dest), B=argc, C=methodNameConstIdx
            std::string method = K(C).stringVal->data;
            BblValue receiver = R(A);
            std::vector<BblValue> args(B);
            for (int i = 0; i < B; i++) args[i] = R(A + 1 + i);

            if (receiver.type == BBL::Type::Vector) {
                BblVec* vec = receiver.vectorVal;
                if (method == "length") R(A) = BblValue::makeInt(static_cast<int64_t>(vec->length()));
                else if (method == "push") { for (auto& a : args) state.packValue(vec, a); R(A) = BblValue::makeNull(); }
                else if (method == "pop") {
                    if (vec->length() == 0) throw BBL::Error{"pop on empty vector"};
                    R(A) = state.readVecElem(vec, vec->length() - 1);
                    vec->data.resize(vec->data.size() - vec->elemSize);
                } else if (method == "clear") { vec->data.clear(); R(A) = BblValue::makeNull(); }
                else if (method == "at") R(A) = state.readVecElem(vec, static_cast<size_t>(args[0].intVal));
                else if (method == "set") { state.writeVecElem(vec, static_cast<size_t>(args[0].intVal), args[1]); R(A) = BblValue::makeNull(); }
                else if (method == "resize") { vec->data.resize(static_cast<size_t>(args[0].intVal) * vec->elemSize, 0); R(A) = BblValue::makeNull(); }
                else throw BBL::Error{"unknown vector method: " + method};
            } else if (receiver.type == BBL::Type::String) {
                BblString* str = receiver.stringVal;
                if (method == "length") R(A) = BblValue::makeInt(static_cast<int64_t>(str->data.size()));
                else if (method == "at") { R(A) = BblValue::makeString(state.intern(std::string(1, str->data[static_cast<size_t>(args[0].intVal)]))); }
                else if (method == "slice") {
                    int64_t start = args[0].intVal;
                    int64_t len = args.size() > 1 ? args[1].intVal : static_cast<int64_t>(str->data.size()) - start;
                    R(A) = BblValue::makeString(state.intern(str->data.substr(static_cast<size_t>(start), static_cast<size_t>(len))));
                } else if (method == "find") {
                    auto pos = str->data.find(args[0].stringVal->data);
                    R(A) = BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
                } else if (method == "contains") R(A) = BblValue::makeBool(str->data.find(args[0].stringVal->data) != std::string::npos);
                else if (method == "starts-with") R(A) = BblValue::makeBool(str->data.starts_with(args[0].stringVal->data));
                else if (method == "ends-with") R(A) = BblValue::makeBool(str->data.ends_with(args[0].stringVal->data));
                else if (method == "upper") { std::string r = str->data; for (auto& c : r) c = static_cast<char>(toupper(c)); R(A) = BblValue::makeString(state.intern(r)); }
                else if (method == "lower") { std::string r = str->data; for (auto& c : r) c = static_cast<char>(tolower(c)); R(A) = BblValue::makeString(state.intern(r)); }
                else if (method == "trim") {
                    auto s = str->data.find_first_not_of(" \t\n\r");
                    auto e = str->data.find_last_not_of(" \t\n\r");
                    R(A) = BblValue::makeString(state.intern(s == std::string::npos ? "" : str->data.substr(s, e - s + 1)));
                } else if (method == "replace") {
                    std::string result = str->data, from = args[0].stringVal->data, to = args[1].stringVal->data;
                    size_t pos = 0;
                    while ((pos = result.find(from, pos)) != std::string::npos) { result.replace(pos, from.size(), to); pos += to.size(); }
                    R(A) = BblValue::makeString(state.intern(result));
                } else if (method == "split") {
                    std::string delim = args[0].stringVal->data;
                    BblTable* tbl = state.allocTable();
                    size_t pos = 0; int64_t idx = 0;
                    while (pos <= str->data.size()) {
                        size_t next = str->data.find(delim, pos);
                        if (next == std::string::npos) next = str->data.size();
                        tbl->set(BblValue::makeInt(idx++), BblValue::makeString(state.intern(str->data.substr(pos, next - pos))));
                        pos = next + delim.size();
                        if (next == str->data.size()) break;
                    }
                    R(A) = BblValue::makeTable(tbl);
                } else throw BBL::Error{"unknown string method: " + method};
            } else if (receiver.type == BBL::Type::Binary) {
                BblBinary* bin = receiver.binaryVal;
                if (method == "length") R(A) = BblValue::makeInt(static_cast<int64_t>(bin->length()));
                else if (method == "at") R(A) = BblValue::makeInt(bin->data[static_cast<size_t>(args[0].intVal)]);
                else if (method == "set") { bin->data[static_cast<size_t>(args[0].intVal)] = static_cast<uint8_t>(args[1].intVal); R(A) = BblValue::makeNull(); }
                else if (method == "slice") {
                    size_t s = static_cast<size_t>(args[0].intVal), l = static_cast<size_t>(args[1].intVal);
                    R(A) = BblValue::makeBinary(state.allocBinary(std::vector<uint8_t>(bin->data.begin() + s, bin->data.begin() + s + l)));
                } else if (method == "resize") { bin->data.resize(static_cast<size_t>(args[0].intVal), 0); R(A) = BblValue::makeNull(); }
                else if (method == "copy-from") {
                    BblBinary* src = args[0].binaryVal;
                    size_t dO = args.size() > 1 ? static_cast<size_t>(args[1].intVal) : 0;
                    size_t sO = args.size() > 2 ? static_cast<size_t>(args[2].intVal) : 0;
                    size_t ln = args.size() > 3 ? static_cast<size_t>(args[3].intVal) : src->length() - sO;
                    std::memcpy(bin->data.data() + dO, src->data.data() + sO, ln);
                    R(A) = BblValue::makeNull();
                } else throw BBL::Error{"unknown binary method: " + method};
            } else if (receiver.type == BBL::Type::Table) {
                BblTable* tbl = receiver.tableVal;
                if (method == "length") R(A) = BblValue::makeInt(static_cast<int64_t>(tbl->length()));
                else if (method == "get") R(A) = tbl->get(args[0]).value_or(args.size() > 1 ? args[1] : BblValue::makeNull());
                else if (method == "set") { tbl->set(args[0], args[1]); R(A) = BblValue::makeNull(); }
                else if (method == "delete") { tbl->del(args[0]); R(A) = BblValue::makeNull(); }
                else if (method == "has") R(A) = BblValue::makeBool(tbl->has(args[0]));
                else if (method == "keys") {
                    BblTable* keys = state.allocTable(); int64_t i = 0;
                    for (auto& k : tbl->order) keys->set(BblValue::makeInt(i++), k);
                    R(A) = BblValue::makeTable(keys);
                } else if (method == "push") {
                    for (auto& a : args) { tbl->set(BblValue::makeInt(tbl->nextIntKey), a); tbl->nextIntKey++; }
                    R(A) = BblValue::makeNull();
                } else if (method == "pop") {
                    bool found = false;
                    for (auto it = tbl->order.rbegin(); it != tbl->order.rend(); ++it) {
                        if (it->type == BBL::Type::Int) {
                            R(A) = tbl->get(*it).value_or(BblValue::makeNull());
                            tbl->del(*it);
                            found = true;
                            break;
                        }
                    }
                    if (!found) throw BBL::Error{"pop: no integer keys"};
                } else if (method == "at") {
                    size_t idx = static_cast<size_t>(args[0].intVal);
                    if (idx >= tbl->order.size()) throw BBL::Error{"table index out of range"};
                    R(A) = tbl->get(tbl->order[idx]).value_or(BblValue::makeNull());
                }
                else throw BBL::Error{"unknown table method: " + method};
            } else if (receiver.type == BBL::Type::UserData) {
                auto it = receiver.userdataVal->desc->methods.find(method);
                if (it == receiver.userdataVal->desc->methods.end())
                    throw BBL::Error{"unknown method '" + method + "'"};
                state.callArgs.clear();
                state.callArgs.push_back(receiver);
                for (auto& a : args) state.callArgs.push_back(a);
                state.hasReturn = false; state.returnValue = BblValue::makeNull();
                it->second(&state);
                R(A) = state.hasReturn ? state.returnValue : BblValue::makeNull();
            } else throw BBL::Error{"cannot call method on " + std::string(::typeName(receiver.type))};
            break;
        }

        case OP_LENGTH: {
            BblValue& obj = R(B);
            if (obj.type == BBL::Type::Vector) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.vectorVal->length()));
            else if (obj.type == BBL::Type::String) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.stringVal->data.size()));
            else if (obj.type == BBL::Type::Binary) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.binaryVal->length()));
            else if (obj.type == BBL::Type::Table) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.tableVal->length()));
            else throw BBL::Error{"cannot get length"};
            break;
        }

        case OP_SIZEOF: {
            std::string tname = K(B).stringVal->data;
            auto dit = state.structDescs.find(tname);
            if (dit == state.structDescs.end()) throw BBL::Error{"unknown struct type: " + tname};
            R(A) = BblValue::makeInt(static_cast<int64_t>(dit->second.totalSize));
            break;
        }

        case OP_EXEC: {
            if (R(B).type != BBL::Type::String) throw BBL::Error{"exec: argument must be string"};
            BblLexer lexer(R(B).stringVal->data.c_str());
            auto nodes = parse(lexer);
            BblValue result = BblValue::makeNull();
            for (auto& n : nodes) result = state.eval(n, state.rootScope);
            R(A) = result;
            break;
        }

        case OP_EXECFILE: {
            if (R(B).type != BBL::Type::String) throw BBL::Error{"execfile: argument must be string"};
            state.execfile(R(B).stringVal->data);
            R(A) = BblValue::makeNull();
            break;
        }

        case OP_TRYBEGIN: {
            VmState::ExHandler handler;
            handler.frameIdx = state.vm->frameCount - 1;
            handler.catchIp = reinterpret_cast<uint8_t*>(frame->ip + sBx);
            handler.localSlot = A;
            handler.stackBase = state.vm->stackTop;
            state.vm->exHandlers.push_back(handler);
            break;
        }
        case OP_TRYEND:
            if (!state.vm->exHandlers.empty()) state.vm->exHandlers.pop_back();
            break;

        default:
            throw BBL::Error{"unknown opcode: " + std::to_string(op)};
        }
      } catch (const BBL::Error& e) {
        if (!state.vm->exHandlers.empty()) {
            auto handler = state.vm->exHandlers.back();
            state.vm->exHandlers.pop_back();
            state.vm->frameCount = handler.frameIdx + 1;
            frame = &state.vm->frames[state.vm->frameCount - 1];
            state.vm->stackTop = handler.stackBase;
            frame->ip = reinterpret_cast<uint32_t*>(handler.catchIp);
            R(handler.localSlot) = BblValue::makeString(state.intern(e.what));
            continue;
        }
        throw;
      }
    }

done:
    #undef R
    #undef K
    return INTERPRET_OK;
}

static bool callValue(BblState& state, CallFrame*& frame, uint8_t base, uint8_t argc, uint8_t destInCaller) {
    BblValue callee = frame->regs[base];
    if (callee.type == BBL::Type::Fn) {
        if (callee.isCFn) {
            state.callArgs.clear();
            for (int i = 0; i < argc; i++)
                state.callArgs.push_back(frame->regs[base + 1 + i]);
            state.hasReturn = false; state.returnValue = BblValue::makeNull();
            callee.cfnVal(&state);
            frame->regs[destInCaller] = state.hasReturn ? state.returnValue : BblValue::makeNull();
            return true;
        }
        if (callee.isClosure) {
            BblClosure* closure = callee.closureVal;
            if (closure->arity != argc)
                throw BBL::Error{"wrong number of arguments: expected " +
                    std::to_string(closure->arity) + " got " + std::to_string(argc)};
            if (state.vm->frameCount >= VM_MAX_FRAMES)
                throw BBL::Error{"stack overflow"};

            CallFrame* newFrame = &state.vm->frames[state.vm->frameCount++];
            newFrame->chunk = &closure->chunk;
            newFrame->ip = closure->chunk.code.data();
            newFrame->regs = state.vm->stackTop;
            newFrame->closure = closure;
            newFrame->numRegs = closure->chunk.numRegs;
            newFrame->line = destInCaller; // repurpose line as return dest in caller

            // Initialize register file
            state.vm->stackTop += closure->chunk.numRegs;
            newFrame->regs[0] = callee; // R[0] = callee
            for (int i = 0; i < argc; i++)
                newFrame->regs[1 + i] = frame->regs[base + 1 + i];

            frame = newFrame;
            return true;
        }
        // Tree-walker BblFn
        BblFn* fn = callee.fnVal;
        BblValue result = state.callFn(fn, &frame->regs[base + 1], static_cast<size_t>(argc), 0);
        frame->regs[destInCaller] = result;
        return true;
    }
    return false;
}
