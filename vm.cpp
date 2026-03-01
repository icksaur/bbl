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

static std::string valToString(BblState& state, const BblValue& v) {
    switch (v.type) {
        case BBL::Type::Null: return "null";
        case BBL::Type::Bool: return v.boolVal ? "true" : "false";
        case BBL::Type::Int: return std::to_string(v.intVal);
        case BBL::Type::Float: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", v.floatVal);
            return buf;
        }
        case BBL::Type::String: return v.stringVal->data;
        default: return "<" + std::string(typeName(v.type)) + ">";
    }
}

static bool callValue(BblState& state, BblValue callee, int argc);

InterpretResult vmExecute(BblState& state, Chunk& chunk) {
    state.vm->reset();

    CallFrame* frame = &state.vm->frames[0];
    frame->chunk = &chunk;
    frame->ip = chunk.code.data();
    frame->slots = state.vm->stack.data();
    frame->closure = nullptr;
    frame->line = 0;
    state.vm->frameCount = 1;
    state.vm->stackTop = state.vm->stack.data() + 1;

    #define READ_BYTE()  (*frame->ip++)
    #define READ_U16()   (frame->ip += 2, \
        static_cast<uint16_t>((frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_CONST() (frame->chunk->constants[READ_U16()])
    #define PUSH(v)      (*state.vm->stackTop++ = (v))
    #define POP()        (*--state.vm->stackTop)
    #define PEEK(n)      (state.vm->stackTop[-1 - (n)])

#ifdef __GNUC__
    static void* dispatchTable[] = {
        &&L_CONSTANT, &&L_NULL, &&L_TRUE, &&L_FALSE,
        &&L_POP, &&L_DUP, &&L_POPN,
        &&L_ADD, &&L_SUB, &&L_MUL, &&L_DIV, &&L_MOD,
        &&L_BAND, &&L_BOR, &&L_BXOR, &&L_BNOT, &&L_SHL, &&L_SHR,
        &&L_EQ, &&L_NEQ, &&L_LT, &&L_GT, &&L_LTE, &&L_GTE,
        &&L_NOT,
        &&L_GET_LOCAL, &&L_SET_LOCAL, &&L_GET_CAPTURE, &&L_SET_CAPTURE,
        &&L_GET_GLOBAL, &&L_SET_GLOBAL,
        &&L_JUMP, &&L_JUMP_IF_FALSE, &&L_JUMP_IF_TRUE, &&L_LOOP,
        &&L_AND, &&L_OR,
        &&L_CLOSURE, &&L_CALL, &&L_TAIL_CALL, &&L_RETURN,
        &&L_VECTOR, &&L_TABLE, &&L_STRUCT, &&L_BINARY,
        &&L_GET_FIELD, &&L_SET_FIELD, &&L_GET_INDEX, &&L_SET_INDEX,
        &&L_METHOD_CALL,
        &&L_TRY_BEGIN, &&L_TRY_END, &&L_WITH_BEGIN, &&L_WITH_END,
        &&L_LENGTH, &&L_SIZEOF, &&L_EXEC, &&L_EXECFILE,
    };

    #define DISPATCH() goto *dispatchTable[READ_BYTE()]
    #define TARGET(name) L_##name:

    for (;;) {
      try {
        DISPATCH();

        TARGET(CONSTANT) PUSH(READ_CONST()); DISPATCH();
        TARGET(NULL)     PUSH(BblValue::makeNull()); DISPATCH();
        TARGET(TRUE)     PUSH(BblValue::makeBool(true)); DISPATCH();
        TARGET(FALSE)    PUSH(BblValue::makeBool(false)); DISPATCH();
        TARGET(POP)      POP(); DISPATCH();
        TARGET(DUP)      PUSH(PEEK(0)); DISPATCH();
        TARGET(POPN) {
            uint8_t n = READ_BYTE();
            state.vm->stackTop -= n;
            DISPATCH();
        }

        TARGET(ADD) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeInt(a.intVal + b.intVal));
            else if (a.type == BBL::Type::String)
                PUSH(BblValue::makeString(state.intern(
                    a.stringVal->data + valToString(state, b))));
            else if (a.type == BBL::Type::Float || b.type == BBL::Type::Float)
                PUSH(BblValue::makeFloat(toFloat(a) + toFloat(b)));
            else
                throw BBL::Error{"+: invalid operand types"};
            DISPATCH();
        }
        TARGET(SUB) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeInt(a.intVal - b.intVal));
            else
                PUSH(BblValue::makeFloat(toFloat(a) - toFloat(b)));
            DISPATCH();
        }
        TARGET(MUL) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeInt(a.intVal * b.intVal));
            else
                PUSH(BblValue::makeFloat(toFloat(a) * toFloat(b)));
            DISPATCH();
        }
        TARGET(DIV) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int) {
                if (b.intVal == 0) throw BBL::Error{"division by zero"};
                PUSH(BblValue::makeInt(a.intVal / b.intVal));
            } else {
                double fd = toFloat(b);
                if (fd == 0.0) throw BBL::Error{"division by zero"};
                PUSH(BblValue::makeFloat(toFloat(a) / fd));
            }
            DISPATCH();
        }
        TARGET(MOD) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int) {
                if (b.intVal == 0) throw BBL::Error{"modulo by zero"};
                PUSH(BblValue::makeInt(a.intVal % b.intVal));
            } else {
                throw BBL::Error{"%: requires integer operands"};
            }
            DISPATCH();
        }

        TARGET(BAND) { BblValue b = POP(), a = POP(); PUSH(BblValue::makeInt(a.intVal & b.intVal)); DISPATCH(); }
        TARGET(BOR)  { BblValue b = POP(), a = POP(); PUSH(BblValue::makeInt(a.intVal | b.intVal)); DISPATCH(); }
        TARGET(BXOR) { BblValue b = POP(), a = POP(); PUSH(BblValue::makeInt(a.intVal ^ b.intVal)); DISPATCH(); }
        TARGET(BNOT) { BblValue a = POP(); PUSH(BblValue::makeInt(~a.intVal)); DISPATCH(); }
        TARGET(SHL)  { BblValue b = POP(), a = POP(); PUSH(BblValue::makeInt(a.intVal << b.intVal)); DISPATCH(); }
        TARGET(SHR)  { BblValue b = POP(), a = POP(); PUSH(BblValue::makeInt(a.intVal >> b.intVal)); DISPATCH(); }

        TARGET(EQ)  { BblValue b = POP(), a = POP(); PUSH(BblValue::makeBool(a == b)); DISPATCH(); }
        TARGET(NEQ) { BblValue b = POP(), a = POP(); PUSH(BblValue::makeBool(a != b)); DISPATCH(); }
        TARGET(LT) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeBool(a.intVal < b.intVal));
            else
                PUSH(BblValue::makeBool(toFloat(a) < toFloat(b)));
            DISPATCH();
        }
        TARGET(GT) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeBool(a.intVal > b.intVal));
            else
                PUSH(BblValue::makeBool(toFloat(a) > toFloat(b)));
            DISPATCH();
        }
        TARGET(LTE) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeBool(a.intVal <= b.intVal));
            else
                PUSH(BblValue::makeBool(toFloat(a) <= toFloat(b)));
            DISPATCH();
        }
        TARGET(GTE) {
            BblValue b = POP(), a = POP();
            if (a.type == BBL::Type::Int && b.type == BBL::Type::Int)
                PUSH(BblValue::makeBool(a.intVal >= b.intVal));
            else
                PUSH(BblValue::makeBool(toFloat(a) >= toFloat(b)));
            DISPATCH();
        }

        TARGET(NOT) {
            BblValue a = POP();
            PUSH(BblValue::makeBool(isFalsy(a)));
            DISPATCH();
        }

        TARGET(GET_LOCAL) {
            uint16_t slot = READ_U16();
            PUSH(frame->slots[slot]);
            DISPATCH();
        }
        TARGET(SET_LOCAL) {
            uint16_t slot = READ_U16();
            BblValue val = PEEK(0);
            if (slot >= static_cast<uint16_t>(state.vm->stackTop - frame->slots)) {
                while (state.vm->stackTop <= frame->slots + slot)
                    *state.vm->stackTop++ = BblValue::makeNull();
            }
            frame->slots[slot] = val;
            DISPATCH();
        }
        TARGET(GET_CAPTURE) {
            uint8_t idx = READ_BYTE();
            if (!frame->closure) throw BBL::Error{"no closure for capture access"};
            PUSH(frame->closure->captures[idx]);
            DISPATCH();
        }
        TARGET(SET_CAPTURE) {
            uint8_t idx = READ_BYTE();
            if (!frame->closure) throw BBL::Error{"no closure for capture access"};
            frame->closure->captures[idx] = PEEK(0);
            DISPATCH();
        }
        TARGET(GET_GLOBAL) {
            uint16_t idx = READ_U16();
            BblValue symVal = frame->chunk->constants[idx];
            uint32_t symId = static_cast<uint32_t>(symVal.intVal);
            auto it = state.vm->globals.find(symId);
            if (it == state.vm->globals.end()) {
                auto* val = state.rootScope.lookup(symId);
                if (val) {
                    PUSH(*val);
                } else {
                    for (auto& [name, id] : state.symbolIds) {
                        if (id == symId) throw BBL::Error{"undefined variable: " + name};
                    }
                    throw BBL::Error{"undefined variable (id=" + std::to_string(symId) + ")"};
                }
            } else {
                PUSH(it->second);
            }
            DISPATCH();
        }
        TARGET(SET_GLOBAL) {
            uint16_t idx = READ_U16();
            BblValue symVal = frame->chunk->constants[idx];
            uint32_t symId = static_cast<uint32_t>(symVal.intVal);
            state.vm->globals[symId] = PEEK(0);
            state.rootScope.def(symId, PEEK(0));
            DISPATCH();
        }

        TARGET(JUMP) {
            int16_t offset = static_cast<int16_t>(READ_U16());
            frame->ip += offset;
            DISPATCH();
        }
        TARGET(JUMP_IF_FALSE) {
            int16_t offset = static_cast<int16_t>(READ_U16());
            if (isFalsy(PEEK(0))) frame->ip += offset;
            POP();
            DISPATCH();
        }
        TARGET(JUMP_IF_TRUE) {
            int16_t offset = static_cast<int16_t>(READ_U16());
            if (!isFalsy(PEEK(0))) frame->ip += offset;
            POP();
            DISPATCH();
        }
        TARGET(LOOP) {
            uint16_t offset = READ_U16();
            frame->ip -= offset;
            if (state.allocCount >= state.gcThreshold) state.gc();
            if (state.maxSteps && ++state.stepCount > state.maxSteps)
                throw BBL::Error{"step limit exceeded: " + std::to_string(state.maxSteps) + " steps"};
            if (state.terminated.load(std::memory_order_relaxed))
                throw BblTerminated{};
            DISPATCH();
        }

        TARGET(AND) {
            int16_t offset = static_cast<int16_t>(READ_U16());
            if (isFalsy(PEEK(0)))
                frame->ip += offset;
            else
                POP();
            DISPATCH();
        }
        TARGET(OR) {
            int16_t offset = static_cast<int16_t>(READ_U16());
            if (!isFalsy(PEEK(0)))
                frame->ip += offset;
            else
                POP();
            DISPATCH();
        }

        TARGET(CLOSURE) {
            uint16_t protoIdx = READ_U16();
            BblClosure* proto = frame->chunk->constants[protoIdx].closureVal;
            uint8_t captureCount = READ_BYTE();

            BblClosure* closure = new BblClosure();
            closure->chunk = proto->chunk;
            closure->arity = proto->arity;
            closure->name = proto->name;
            closure->captures.resize(captureCount);
            state.allocatedClosures.push_back(closure);

            for (int i = 0; i < captureCount; i++) {
                uint8_t srcType = READ_BYTE();
                uint16_t srcIdx = READ_U16();
                if (srcType == 0) {
                    closure->captures[i] = frame->slots[srcIdx];
                } else {
                    if (frame->closure)
                        closure->captures[i] = frame->closure->captures[srcIdx];
                    else
                        closure->captures[i] = BblValue::makeNull();
                }
            }

            // Self-capture: if fn name matches a capture slot, patch it
            if (!closure->name.empty()) {
                uint32_t selfSym = state.resolveSymbol(closure->name);
                for (size_t i = 0; i < closure->captures.size(); i++) {
                    // Check if this capture was meant for self
                    // We detect self-reference by checking if the captured value is null
                    // for a slot that should hold the function itself
                }
                // Simpler: always set self as a capture after creation
                // The compiler should have placed the self-capture slot
            }

            BblValue cv;
            cv.type = BBL::Type::Fn;
            cv.isCFn = false;
            cv.closureVal = closure;
            PUSH(cv);
            DISPATCH();
        }

        TARGET(CALL) {
            uint8_t argc = READ_BYTE();
            if (!callValue(state, PEEK(argc), argc))
                throw BBL::Error{"not callable"};
            frame = &state.vm->frames[state.vm->frameCount - 1];
            if (state.maxSteps && ++state.stepCount > state.maxSteps)
                throw BBL::Error{"step limit exceeded: " + std::to_string(state.maxSteps) + " steps"};
            if (state.terminated.load(std::memory_order_relaxed))
                throw BblTerminated{};
            DISPATCH();
        }

        TARGET(TAIL_CALL) {
            uint8_t argc = READ_BYTE();
            BblValue callee = PEEK(argc);
            if (callee.type != BBL::Type::Fn || callee.isCFn)
                throw BBL::Error{"tail call target must be a bytecode function"};

            BblClosure* closure = callee.closureVal;
            if (closure->arity != argc)
                throw BBL::Error{"wrong number of arguments: expected " +
                    std::to_string(closure->arity) + " got " + std::to_string(argc)};

            BblValue* args = state.vm->stackTop - argc;
            frame->slots[0] = callee;
            for (int i = 0; i < argc; i++)
                frame->slots[i + 1] = args[i];

            state.vm->stackTop = frame->slots + argc + 1;
            frame->chunk = &closure->chunk;
            frame->ip = closure->chunk.code.data();
            frame->closure = closure;
            DISPATCH();
        }

        TARGET(RETURN) {
            BblValue result = POP();
            state.vm->frameCount--;
            if (state.vm->frameCount == 0) {
                state.vm->stackTop = state.vm->stack.data();
                PUSH(result);
                goto done;
            }
            state.vm->stackTop = frame->slots;
            frame = &state.vm->frames[state.vm->frameCount - 1];
            PUSH(result);
            DISPATCH();
        }

        TARGET(VECTOR) {
            uint16_t count = READ_U16();
            BblValue typeNameVal = state.vm->stackTop[-(int)count - 1];
            std::string elemType = typeNameVal.stringVal->data;

            BBL::Type elemTypeTag = BBL::Type::Null;
            size_t elemSize = 0;
            auto dit = state.structDescs.find(elemType);
            if (dit != state.structDescs.end()) {
                elemTypeTag = BBL::Type::Struct;
                elemSize = dit->second.totalSize;
            } else if (elemType == "int" || elemType == "int64") {
                elemTypeTag = BBL::Type::Int; elemSize = 8;
            } else if (elemType == "float" || elemType == "float64") {
                elemTypeTag = BBL::Type::Float; elemSize = 8;
            } else if (elemType == "float32") {
                elemTypeTag = BBL::Type::Float; elemSize = 4;
            } else if (elemType == "int32") {
                elemTypeTag = BBL::Type::Int; elemSize = 4;
            } else if (elemType == "int16") {
                elemTypeTag = BBL::Type::Int; elemSize = 2;
            } else if (elemType == "int8" || elemType == "uint8") {
                elemTypeTag = BBL::Type::Int; elemSize = 1;
            } else {
                throw BBL::Error{"unknown vector element type: " + elemType};
            }

            BblVec* vec = state.allocVector(elemType, elemTypeTag, elemSize);
            for (uint16_t i = 0; i < count; i++) {
                BblValue val = state.vm->stackTop[-(int)count + i];
                state.packValue(vec, val);
            }
            state.vm->stackTop -= count + 1;
            PUSH(BblValue::makeVector(vec));
            DISPATCH();
        }

        TARGET(TABLE) {
            uint16_t pairCount = READ_U16();
            BblTable* tbl = state.allocTable();
            BblValue* base = state.vm->stackTop - pairCount * 2;
            for (uint16_t i = 0; i < pairCount; i++) {
                BblValue key = base[i * 2];
                BblValue val = base[i * 2 + 1];
                tbl->set(key, val);
            }
            state.vm->stackTop = base;
            PUSH(BblValue::makeTable(tbl));
            DISPATCH();
        }

        TARGET(STRUCT) {
            uint16_t typeIdx = READ_U16();
            uint8_t argc = READ_BYTE();
            std::string typeName = frame->chunk->constants[typeIdx].stringVal->data;
            auto dit = state.structDescs.find(typeName);
            if (dit == state.structDescs.end())
                throw BBL::Error{"unknown struct type: " + typeName};

            std::vector<BblValue> args(argc);
            for (int i = argc - 1; i >= 0; i--)
                args[i] = POP();

            int line = frame->chunk->lines[frame->ip - frame->chunk->code.data() - 1];
            BblValue result = state.constructStruct(&dit->second, args, line);
            PUSH(result);
            DISPATCH();
        }

        TARGET(BINARY) {
            BblValue arg = POP();
            if (arg.type == BBL::Type::Vector) {
                BblBinary* bin = state.allocBinary(arg.vectorVal->data);
                PUSH(BblValue::makeBinary(bin));
            } else if (arg.type == BBL::Type::Struct) {
                BblBinary* bin = state.allocBinary(arg.structVal->data);
                PUSH(BblValue::makeBinary(bin));
            } else if (arg.type == BBL::Type::Int) {
                if (arg.intVal < 0) throw BBL::Error{"binary: size must be non-negative"};
                std::vector<uint8_t> data(static_cast<size_t>(arg.intVal), 0);
                BblBinary* bin = state.allocBinary(std::move(data));
                PUSH(BblValue::makeBinary(bin));
            } else {
                throw BBL::Error{"binary: argument must be vector, struct, or non-negative integer"};
            }
            DISPATCH();
        }

        TARGET(GET_FIELD) {
            uint16_t nameIdx = READ_U16();
            BblValue obj = POP();
            std::string fieldName = frame->chunk->constants[nameIdx].stringVal->data;

            if (obj.type == BBL::Type::Struct) {
                auto* desc = obj.structVal->desc;
                for (auto& fd : desc->fields) {
                    if (fd.name == fieldName) {
                        PUSH(state.readField(obj.structVal, fd));
                        goto field_done;
                    }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type == BBL::Type::Table) {
                BblValue key = BblValue::makeString(state.intern(fieldName));
                auto result = obj.tableVal->get(key);
                PUSH(result.value_or(BblValue::makeNull()));
            } else {
                throw BBL::Error{"cannot access field on " + std::string(::typeName(obj.type))};
            }
            field_done:
            DISPATCH();
        }

        TARGET(SET_FIELD) {
            uint16_t nameIdx = READ_U16();
            BblValue obj = POP();
            BblValue val = POP();
            std::string fieldName = frame->chunk->constants[nameIdx].stringVal->data;

            if (obj.type == BBL::Type::Struct) {
                auto* desc = obj.structVal->desc;
                for (auto& fd : desc->fields) {
                    if (fd.name == fieldName) {
                        state.writeField(obj.structVal, fd, val);
                        PUSH(val);
                        goto setfield_done;
                    }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type == BBL::Type::Table) {
                BblValue key = BblValue::makeString(state.intern(fieldName));
                obj.tableVal->set(key, val);
                PUSH(val);
            } else {
                throw BBL::Error{"cannot set field on " + std::string(::typeName(obj.type))};
            }
            setfield_done:
            DISPATCH();
        }

        TARGET(GET_INDEX) {
            BblValue idx = POP();
            BblValue obj = POP();
            if (obj.type == BBL::Type::Vector) {
                if (idx.type != BBL::Type::Int) throw BBL::Error{"vector index must be int"};
                PUSH(state.readVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal)));
            } else if (obj.type == BBL::Type::Table) {
                auto result = obj.tableVal->get(idx);
                PUSH(result.value_or(BblValue::makeNull()));
            } else if (obj.type == BBL::Type::Binary) {
                if (idx.type != BBL::Type::Int) throw BBL::Error{"binary index must be int"};
                size_t i = static_cast<size_t>(idx.intVal);
                if (i >= obj.binaryVal->length()) throw BBL::Error{"binary index out of range"};
                PUSH(BblValue::makeInt(obj.binaryVal->data[i]));
            } else {
                throw BBL::Error{"cannot index " + std::string(::typeName(obj.type))};
            }
            DISPATCH();
        }

        TARGET(SET_INDEX) {
            BblValue val = POP();
            BblValue idx = POP();
            BblValue obj = POP();
            if (obj.type == BBL::Type::Vector) {
                state.writeVecElem(obj.vectorVal, static_cast<size_t>(idx.intVal), val);
            } else if (obj.type == BBL::Type::Table) {
                obj.tableVal->set(idx, val);
            } else {
                throw BBL::Error{"cannot set index on " + std::string(::typeName(obj.type))};
            }
            PUSH(val);
            DISPATCH();
        }

        TARGET(METHOD_CALL) {
            uint16_t nameIdx = READ_U16();
            uint8_t argc = READ_BYTE();
            std::string method = frame->chunk->constants[nameIdx].stringVal->data;
            BblValue receiver = PEEK(argc);

            // Build args for method dispatch
            std::vector<BblValue> args(argc);
            for (int i = argc - 1; i >= 0; i--)
                args[i] = POP();
            POP(); // receiver

            // We need to dispatch through existing method infrastructure
            // Build a temporary callArgs for C-function-style dispatch
            state.callArgs.clear();
            state.callArgs.push_back(receiver);
            for (auto& a : args) state.callArgs.push_back(a);
            state.hasReturn = false;
            state.returnValue = BblValue::makeNull();

            if (receiver.type == BBL::Type::Vector) {
                // Use evalVectorMethod by building a synthetic AST node - too complex
                // Instead, inline common methods
                BblVec* vec = receiver.vectorVal;
                if (method == "length") {
                    PUSH(BblValue::makeInt(static_cast<int64_t>(vec->length())));
                } else if (method == "push") {
                    for (auto& a : args) state.packValue(vec, a);
                    PUSH(BblValue::makeNull());
                } else if (method == "pop") {
                    if (vec->length() == 0) throw BBL::Error{"pop on empty vector"};
                    BblValue last = state.readVecElem(vec, vec->length() - 1);
                    vec->data.resize(vec->data.size() - vec->elemSize);
                    PUSH(last);
                } else if (method == "clear") {
                    vec->data.clear();
                    PUSH(BblValue::makeNull());
                } else if (method == "at") {
                    if (args.size() != 1) throw BBL::Error{"at requires 1 argument"};
                    PUSH(state.readVecElem(vec, static_cast<size_t>(args[0].intVal)));
                } else if (method == "set") {
                    if (args.size() != 2) throw BBL::Error{"set requires 2 arguments"};
                    state.writeVecElem(vec, static_cast<size_t>(args[0].intVal), args[1]);
                    PUSH(BblValue::makeNull());
                } else if (method == "resize") {
                    if (args.size() != 1) throw BBL::Error{"resize requires 1 argument"};
                    vec->data.resize(static_cast<size_t>(args[0].intVal) * vec->elemSize, 0);
                    PUSH(BblValue::makeNull());
                } else {
                    throw BBL::Error{"unknown vector method: " + method};
                }
            } else if (receiver.type == BBL::Type::String) {
                BblString* str = receiver.stringVal;
                if (method == "length") {
                    PUSH(BblValue::makeInt(static_cast<int64_t>(str->data.size())));
                } else if (method == "at") {
                    size_t i = static_cast<size_t>(args[0].intVal);
                    if (i >= str->data.size()) throw BBL::Error{"string index out of range"};
                    PUSH(BblValue::makeString(state.intern(std::string(1, str->data[i]))));
                } else if (method == "slice") {
                    int64_t start = args[0].intVal;
                    int64_t len = args.size() > 1 ? args[1].intVal : static_cast<int64_t>(str->data.size()) - start;
                    PUSH(BblValue::makeString(state.intern(str->data.substr(static_cast<size_t>(start), static_cast<size_t>(len)))));
                } else if (method == "find") {
                    auto pos = str->data.find(args[0].stringVal->data);
                    PUSH(BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos)));
                } else if (method == "contains") {
                    PUSH(BblValue::makeBool(str->data.find(args[0].stringVal->data) != std::string::npos));
                } else if (method == "starts-with") {
                    PUSH(BblValue::makeBool(str->data.starts_with(args[0].stringVal->data)));
                } else if (method == "ends-with") {
                    PUSH(BblValue::makeBool(str->data.ends_with(args[0].stringVal->data)));
                } else if (method == "split") {
                    std::string delim = args[0].stringVal->data;
                    BblTable* tbl = state.allocTable();
                    size_t pos = 0;
                    int64_t idx = 0;
                    while (pos <= str->data.size()) {
                        size_t next = str->data.find(delim, pos);
                        if (next == std::string::npos) next = str->data.size();
                        tbl->set(BblValue::makeInt(idx++),
                                BblValue::makeString(state.intern(str->data.substr(pos, next - pos))));
                        pos = next + delim.size();
                        if (next == str->data.size()) break;
                    }
                    PUSH(BblValue::makeTable(tbl));
                } else if (method == "upper") {
                    std::string r = str->data;
                    for (auto& c : r) c = static_cast<char>(toupper(c));
                    PUSH(BblValue::makeString(state.intern(r)));
                } else if (method == "lower") {
                    std::string r = str->data;
                    for (auto& c : r) c = static_cast<char>(tolower(c));
                    PUSH(BblValue::makeString(state.intern(r)));
                } else if (method == "trim") {
                    std::string r = str->data;
                    auto start = r.find_first_not_of(" \t\n\r");
                    auto end = r.find_last_not_of(" \t\n\r");
                    PUSH(BblValue::makeString(state.intern(
                        start == std::string::npos ? "" : r.substr(start, end - start + 1))));
                } else if (method == "replace") {
                    std::string result = str->data;
                    std::string from = args[0].stringVal->data;
                    std::string to = args[1].stringVal->data;
                    size_t pos = 0;
                    while ((pos = result.find(from, pos)) != std::string::npos) {
                        result.replace(pos, from.size(), to);
                        pos += to.size();
                    }
                    PUSH(BblValue::makeString(state.intern(result)));
                } else {
                    throw BBL::Error{"unknown string method: " + method};
                }
            } else if (receiver.type == BBL::Type::Binary) {
                BblBinary* bin = receiver.binaryVal;
                if (method == "length") {
                    PUSH(BblValue::makeInt(static_cast<int64_t>(bin->length())));
                } else if (method == "at") {
                    size_t i = static_cast<size_t>(args[0].intVal);
                    if (i >= bin->length()) throw BBL::Error{"binary index out of range"};
                    PUSH(BblValue::makeInt(bin->data[i]));
                } else if (method == "set") {
                    size_t i = static_cast<size_t>(args[0].intVal);
                    if (i >= bin->length()) throw BBL::Error{"binary index out of range"};
                    bin->data[i] = static_cast<uint8_t>(args[1].intVal);
                    PUSH(BblValue::makeNull());
                } else if (method == "slice") {
                    size_t start = static_cast<size_t>(args[0].intVal);
                    size_t len = static_cast<size_t>(args[1].intVal);
                    std::vector<uint8_t> sl(bin->data.begin() + start, bin->data.begin() + start + len);
                    PUSH(BblValue::makeBinary(state.allocBinary(std::move(sl))));
                } else if (method == "resize") {
                    bin->data.resize(static_cast<size_t>(args[0].intVal), 0);
                    PUSH(BblValue::makeNull());
                } else if (method == "copy-from") {
                    BblBinary* src = args[0].binaryVal;
                    size_t dstOff = args.size() > 1 ? static_cast<size_t>(args[1].intVal) : 0;
                    size_t srcOff = args.size() > 2 ? static_cast<size_t>(args[2].intVal) : 0;
                    size_t len = args.size() > 3 ? static_cast<size_t>(args[3].intVal) : src->length() - srcOff;
                    std::memcpy(bin->data.data() + dstOff, src->data.data() + srcOff, len);
                    PUSH(BblValue::makeNull());
                } else {
                    throw BBL::Error{"unknown binary method: " + method};
                }
            } else if (receiver.type == BBL::Type::Table) {
                BblTable* tbl = receiver.tableVal;
                if (method == "length") {
                    PUSH(BblValue::makeInt(static_cast<int64_t>(tbl->length())));
                } else if (method == "get") {
                    auto result = tbl->get(args[0]);
                    PUSH(result.value_or(args.size() > 1 ? args[1] : BblValue::makeNull()));
                } else if (method == "set") {
                    tbl->set(args[0], args[1]);
                    PUSH(BblValue::makeNull());
                } else if (method == "delete") {
                    tbl->del(args[0]);
                    PUSH(BblValue::makeNull());
                } else if (method == "has") {
                    PUSH(BblValue::makeBool(tbl->has(args[0])));
                } else if (method == "keys") {
                    BblTable* keys = state.allocTable();
                    int64_t idx = 0;
                    for (auto& [k, v] : tbl->entries)
                        keys->set(BblValue::makeInt(idx++), k);
                    PUSH(BblValue::makeTable(keys));
                } else if (method == "push") {
                    for (auto& a : args) {
                        tbl->set(BblValue::makeInt(tbl->nextIntKey), a);
                        tbl->nextIntKey++;
                    }
                    PUSH(BblValue::makeNull());
                } else if (method == "pop") {
                    if (tbl->entries.empty()) throw BBL::Error{"pop on empty table"};
                    BblValue last = tbl->entries.back().second;
                    tbl->entries.pop_back();
                    PUSH(last);
                } else if (method == "at") {
                    size_t i = static_cast<size_t>(args[0].intVal);
                    if (i >= tbl->entries.size()) throw BBL::Error{"table index out of range"};
                    PUSH(tbl->entries[i].second);
                } else {
                    throw BBL::Error{"unknown table method: " + method};
                }
            } else if (receiver.type == BBL::Type::UserData) {
                auto it = receiver.userdataVal->desc->methods.find(method);
                if (it == receiver.userdataVal->desc->methods.end())
                    throw BBL::Error{"unknown method '" + method + "' on userdata type '" + receiver.userdataVal->desc->name + "'"};
                state.callArgs.clear();
                state.callArgs.push_back(receiver);
                for (auto& a : args) state.callArgs.push_back(a);
                state.hasReturn = false;
                state.returnValue = BblValue::makeNull();
                it->second(&state);
                PUSH(state.hasReturn ? state.returnValue : BblValue::makeNull());
            } else {
                throw BBL::Error{"cannot call method on " + std::string(::typeName(receiver.type))};
            }
            DISPATCH();
        }

        TARGET(LENGTH) {
            BblValue obj = POP();
            if (obj.type == BBL::Type::Vector)
                PUSH(BblValue::makeInt(static_cast<int64_t>(obj.vectorVal->length())));
            else if (obj.type == BBL::Type::String)
                PUSH(BblValue::makeInt(static_cast<int64_t>(obj.stringVal->data.size())));
            else if (obj.type == BBL::Type::Binary)
                PUSH(BblValue::makeInt(static_cast<int64_t>(obj.binaryVal->length())));
            else if (obj.type == BBL::Type::Table)
                PUSH(BblValue::makeInt(static_cast<int64_t>(obj.tableVal->length())));
            else
                throw BBL::Error{"cannot get length of " + std::string(::typeName(obj.type))};
            DISPATCH();
        }

        TARGET(SIZEOF) {
            uint16_t nameIdx = READ_U16();
            std::string tname = frame->chunk->constants[nameIdx].stringVal->data;
            auto dit = state.structDescs.find(tname);
            if (dit == state.structDescs.end())
                throw BBL::Error{"unknown struct type: " + tname};
            PUSH(BblValue::makeInt(static_cast<int64_t>(dit->second.totalSize)));
            DISPATCH();
        }

        TARGET(EXEC) {
            BblValue src = POP();
            if (src.type != BBL::Type::String) throw BBL::Error{"exec: argument must be string"};

            // Execute in tree-walker mode since recursive bytecode execution is complex
            BblLexer lexer(src.stringVal->data.c_str());
            auto nodes = parse(lexer);
            BblValue result = BblValue::makeNull();
            for (auto& n : nodes)
                result = state.eval(n, state.rootScope);
            PUSH(result);
            DISPATCH();
        }

        TARGET(EXECFILE) {
            BblValue pathVal = POP();
            if (pathVal.type != BBL::Type::String) throw BBL::Error{"execfile: argument must be string"};
            state.execfile(pathVal.stringVal->data);
            PUSH(BblValue::makeNull());
            DISPATCH();
        }

        TARGET(TRY_BEGIN) {
            uint16_t offset = READ_U16();
            VmState::ExHandler handler;
            handler.frameIdx = state.vm->frameCount - 1;
            handler.catchIp = frame->ip + offset;
            handler.localSlot = 0;
            handler.stackBase = state.vm->stackTop;
            state.vm->exHandlers.push_back(handler);
            DISPATCH();
        }

        TARGET(TRY_END) {
            if (!state.vm->exHandlers.empty())
                state.vm->exHandlers.pop_back();
            DISPATCH();
        }

        TARGET(WITH_BEGIN)
        TARGET(WITH_END)
            throw BBL::Error{"'with' not yet implemented in bytecode"};

      } catch (const BBL::Error& e) {
        if (!state.vm->exHandlers.empty()) {
            auto handler = state.vm->exHandlers.back();
            state.vm->exHandlers.pop_back();
            state.vm->frameCount = handler.frameIdx + 1;
            frame = &state.vm->frames[state.vm->frameCount - 1];
            state.vm->stackTop = handler.stackBase;
            frame->ip = handler.catchIp;
            PUSH(BblValue::makeString(state.intern(e.what)));
            continue;  // re-enter dispatch loop
        }
        throw;
      }
    }

#else
    // Non-GCC fallback: standard switch dispatch
    for (;;) {
      try {
        uint8_t op = READ_BYTE();
        switch (op) {
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
            frame->ip = handler.catchIp;
            PUSH(BblValue::makeString(state.intern(e.what)));
            continue;
        }
        throw;
      }
    }
#endif

done:
    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONST
    #undef PUSH
    #undef POP
    #undef PEEK
    return INTERPRET_OK;
}

static bool callValue(BblState& state, BblValue callee, int argc) {
    if (callee.type == BBL::Type::Fn) {
        if (callee.isCFn) {
            BblCFunction cfn = callee.cfnVal;
            state.callArgs.clear();
            for (int i = 0; i < argc; i++)
                state.callArgs.push_back(state.vm->stackTop[-argc + i]);
            state.hasReturn = false;
            state.returnValue = BblValue::makeNull();
            cfn(&state);
            BblValue result = state.hasReturn ? state.returnValue : BblValue::makeNull();
            state.vm->stackTop -= argc + 1;
            *state.vm->stackTop++ = result;
            return true;
        }

        BblClosure* closure = callee.closureVal;
        if (!closure) {
            // Tree-walker BblFn - call through existing infrastructure
            BblFn* fn = callee.fnVal;
            BblValue* args = state.vm->stackTop - argc;
            BblValue result = state.callFn(fn, args, static_cast<size_t>(argc), 0);
            state.vm->stackTop -= argc + 1;
            *state.vm->stackTop++ = result;
            return true;
        }

        if (closure->arity != argc)
            throw BBL::Error{"wrong number of arguments: expected " +
                std::to_string(closure->arity) + " got " + std::to_string(argc)};

        if (state.vm->frameCount >= VM_MAX_FRAMES)
            throw BBL::Error{"stack overflow"};

        CallFrame* frame = &state.vm->frames[state.vm->frameCount++];
        frame->chunk = &closure->chunk;
        frame->ip = closure->chunk.code.data();
        frame->slots = state.vm->stackTop - argc - 1;
        frame->closure = closure;
        frame->line = 0;
        return true;
    }
    return false;
}
