#include "vm.h"
#include "bbl.h"
#include "compiler.h"
#include "jit.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

size_t Chunk::addConstant(const BblValue& val) {
    constants.push_back(val);
    return constants.size() - 1;
}

static bool isFalsy(const BblValue& v) {
    if (v.type() == BBL::Type::Null) return true;
    if (v.type() == BBL::Type::Bool) return !v.boolVal();
    if (v.type() == BBL::Type::Int) return v.intVal() == 0;
    return false;
}

static double toFloat(const BblValue& v) {
    if (v.type() == BBL::Type::Int) return static_cast<double>(v.intVal());
    if (v.type() == BBL::Type::Float) return v.floatVal();
    throw BBL::Error{"expected number"};
}

static std::string valToStr(BblState& state, const BblValue& v) {
    switch (v.type()) {
        case BBL::Type::Null: return "null";
        case BBL::Type::Bool: return v.boolVal() ? "true" : "false";
        case BBL::Type::Int: return std::to_string(v.intVal());
        case BBL::Type::Float: { char b[64]; snprintf(b, 64, "%g", v.floatVal()); return b; }
        case BBL::Type::String: return v.stringVal()->data;
        default: return "<" + std::string(typeName(v.type())) + ">";
    }
}

static bool callValue(BblState& state, CallFrame*& frame, uint8_t base, uint8_t argc, uint8_t destInCaller);

InterpretResult vmExecute(BblState& state, Chunk& chunk) {
    state.vm->reset();

    CallFrame* frame = &state.vm->frames[0];
    frame->chunk = &chunk;
    frame->ip = chunk.code.data();
    frame->regs = state.vm->stack.data();
    frame->regs[0] = BblValue::makeNull();
    frame->returnDest = 0;
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
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int)
                R(A) = BblValue::makeInt(rb.intVal() + rc.intVal());
            else if (rb.type() == BBL::Type::String) {
                if (A == B) {
                    rb.stringVal()->data += valToStr(state, rc);
                } else {
                    R(A) = BblValue::makeString(state.allocString(rb.stringVal()->data + valToStr(state, rc)));
                }
            }
            else
                R(A) = BblValue::makeFloat(toFloat(rb) + toFloat(rc));
            break;
        }
        case OP_ADDK: {
            BblValue& rb = R(B); BblValue& kc = K(C);
            if (rb.type() == BBL::Type::Int && kc.type() == BBL::Type::Int)
                R(A) = BblValue::makeInt(rb.intVal() + kc.intVal());
            else if (rb.type() == BBL::Type::String)
                R(A) = BblValue::makeString(state.allocString(rb.stringVal()->data + valToStr(state, kc)));
            else
                R(A) = BblValue::makeFloat(toFloat(rb) + toFloat(kc));
            break;
        }
        case OP_ADDI: {
            BblValue& ra = R(A);
            if (ra.type() == BBL::Type::Int)
                ra = BblValue::makeInt(ra.intVal() + static_cast<int64_t>(sBx));
            else
                R(A) = BblValue::makeFloat(toFloat(ra) + static_cast<double>(sBx));
            break;
        }
        case OP_SUB: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeInt(rb.intVal() - rc.intVal());
            else R(A) = BblValue::makeFloat(toFloat(rb) - toFloat(rc));
            break;
        }
        case OP_SUBI: {
            BblValue& ra = R(A);
            if (ra.type() == BBL::Type::Int)
                ra = BblValue::makeInt(ra.intVal() - static_cast<int64_t>(sBx));
            else
                R(A) = BblValue::makeFloat(toFloat(ra) - static_cast<double>(sBx));
            break;
        }
        case OP_MUL: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeInt(rb.intVal() * rc.intVal());
            else R(A) = BblValue::makeFloat(toFloat(rb) * toFloat(rc));
            break;
        }
        case OP_DIV: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) {
                if (rc.intVal() == 0) throw BBL::Error{"division by zero"};
                R(A) = BblValue::makeInt(rb.intVal() / rc.intVal());
            } else {
                double d = toFloat(rc); if (d == 0.0) throw BBL::Error{"division by zero"};
                R(A) = BblValue::makeFloat(toFloat(rb) / d);
            }
            break;
        }
        case OP_MOD: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rc.intVal() == 0) throw BBL::Error{"modulo by zero"};
            R(A) = BblValue::makeInt(rb.intVal() % rc.intVal());
            break;
        }

        case OP_BAND: R(A) = BblValue::makeInt(R(B).intVal() & R(C).intVal()); break;
        case OP_BOR:  R(A) = BblValue::makeInt(R(B).intVal() | R(C).intVal()); break;
        case OP_BXOR: R(A) = BblValue::makeInt(R(B).intVal() ^ R(C).intVal()); break;
        case OP_BNOT: R(A) = BblValue::makeInt(~R(B).intVal()); break;
        case OP_SHL:  R(A) = BblValue::makeInt(R(B).intVal() << R(C).intVal()); break;
        case OP_SHR:  R(A) = BblValue::makeInt(R(B).intVal() >> R(C).intVal()); break;

        case OP_EQ:  R(A) = BblValue::makeBool(R(B) == R(C)); break;
        case OP_NEQ: R(A) = BblValue::makeBool(R(B) != R(C)); break;
        case OP_LT: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal() < rc.intVal());
            else R(A) = BblValue::makeBool(toFloat(rb) < toFloat(rc));
            break;
        }
        case OP_GT: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal() > rc.intVal());
            else R(A) = BblValue::makeBool(toFloat(rb) > toFloat(rc));
            break;
        }
        case OP_LTE: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal() <= rc.intVal());
            else R(A) = BblValue::makeBool(toFloat(rb) <= toFloat(rc));
            break;
        }
        case OP_GTE: {
            BblValue& rb = R(B); BblValue& rc = R(C);
            if (rb.type() == BBL::Type::Int && rc.type() == BBL::Type::Int) R(A) = BblValue::makeBool(rb.intVal() >= rc.intVal());
            else R(A) = BblValue::makeBool(toFloat(rb) >= toFloat(rc));
            break;
        }

        // Fused compare-and-jump: iABC where A,B are registers, sBx encoded in B<<8|C as signed
        // Actually use next instruction for jump target: LTJMP A B followed by JMP sBx
        // Simpler: LTJMP is iABC, if R[A] < R[B] then skip next instruction
        case OP_LTJMP: {
            BblValue& ra = R(A); BblValue& rb = R(B);
            bool cond;
            if (ra.type() == BBL::Type::Int && rb.type() == BBL::Type::Int) cond = ra.intVal() < rb.intVal();
            else cond = toFloat(ra) < toFloat(rb);
            if (cond) frame->ip++; // skip the exit JMP, continue to body
            break;
        }
        case OP_LEJMP: {
            BblValue& ra = R(A); BblValue& rb = R(B);
            bool cond;
            if (ra.type() == BBL::Type::Int && rb.type() == BBL::Type::Int) cond = ra.intVal() <= rb.intVal();
            else cond = toFloat(ra) <= toFloat(rb);
            if (cond) frame->ip++;
            break;
        }
        case OP_GTJMP: {
            BblValue& ra = R(A); BblValue& rb = R(B);
            bool cond;
            if (ra.type() == BBL::Type::Int && rb.type() == BBL::Type::Int) cond = ra.intVal() > rb.intVal();
            else cond = toFloat(ra) > toFloat(rb);
            if (cond) frame->ip++;
            break;
        }
        case OP_GEJMP: {
            BblValue& ra = R(A); BblValue& rb = R(B);
            bool cond;
            if (ra.type() == BBL::Type::Int && rb.type() == BBL::Type::Int) cond = ra.intVal() >= rb.intVal();
            else cond = toFloat(ra) >= toFloat(rb);
            if (cond) frame->ip++;
            break;
        }

        case OP_NOT: R(A) = BblValue::makeBool(isFalsy(R(B))); break;
        case OP_MOVE: R(A) = R(B); break;

        case OP_GETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal());
            auto it = state.vm->globals.find(symId);
            if (it != state.vm->globals.end()) { R(A) = it->second; break; }
            for (auto& [name, id] : state.symbolIds)
                if (id == symId) throw BBL::Error{"undefined variable: " + name};
            throw BBL::Error{"undefined variable"};
        }
        case OP_SETGLOBAL: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal());
            state.vm->setGlobal(symId, R(A));
            break;
        }
        case OP_ENVGET: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal());
            BblTable* env = nullptr;
            if (R(0).type() == BBL::Type::Fn && R(0).isClosure()) env = R(0).closureVal()->env;
            if (!env) env = state.currentEnv;
            if (env) {
                auto nit = state.symbolNames.find(symId);
                if (nit != state.symbolNames.end()) {
                    auto v = env->get(BblValue::makeString(nit->second));
                    if (v) { R(A) = *v; break; }
                }
            }
            auto it = state.vm->globals.find(symId);
            if (it != state.vm->globals.end()) { R(A) = it->second; break; }
            throw BBL::Error{"undefined variable"};
        }
        case OP_ENVSET: {
            uint32_t symId = static_cast<uint32_t>(K(Bx).intVal());
            BblTable* env = nullptr;
            if (R(0).type() == BBL::Type::Fn && R(0).isClosure()) env = R(0).closureVal()->env;
            if (!env) env = state.currentEnv;
            if (env) {
                auto nit = state.symbolNames.find(symId);
                if (nit != state.symbolNames.end()) {
                    env->set(BblValue::makeString(nit->second), R(A));
                    break;
                }
            }
            state.vm->setGlobal(symId, R(A));
            break;
        }
        case OP_GETCAPTURE:
            if (!R(0).isClosure()) throw BBL::Error{"no closure"};
            R(A) = R(0).closureVal()->captures[B];
            break;
        case OP_SETCAPTURE:
            if (!R(0).isClosure()) throw BBL::Error{"no closure"};
            R(0).closureVal()->captures[B] = R(A);
            break;

        case OP_JMP: frame->ip += sBx; break;
        case OP_JMPFALSE: if (isFalsy(R(A))) frame->ip += sBx; break;
        case OP_JMPTRUE:  if (!isFalsy(R(A))) frame->ip += sBx; break;
        case OP_LOOP: {
            size_t loopPc = static_cast<size_t>(frame->ip - frame->chunk->code.data() - 1);
            frame->ip -= sBx;

            auto& lt = frame->chunk->loopTraces[static_cast<uint32_t>(loopPc)];
            if (!lt.compiled && !lt.blacklisted) {
                lt.hotCount++;
                if (lt.hotCount >= 64) {
                    Trace trace = recordTrace(state, *frame->chunk, loopPc, frame->regs);
                    if (trace.valid) {
                        optimizeTrace(state, trace);
                        JitCode jit = compileTrace(state, trace);
                        if (jit.buf) {
                            lt.code = jit.buf;
                            lt.capacity = jit.capacity;
                            lt.compiled = true;
                            lt.snapshots = new std::vector<Snapshot>(std::move(trace.snapshots));
                            if (!trace.sunkAllocs.empty())
                                lt.sunkAllocs = new std::vector<SunkAllocation>(std::move(trace.sunkAllocs));
                        } else {
                            lt.blacklisted = true;
                        }
                    } else {
                        lt.blacklisted = true;
                    }
                    lt.hotCount = 0;
                }
            }
            if (lt.compiled && lt.code) {
                JitCode traceJit;
                traceJit.buf = static_cast<uint8_t*>(lt.code);
                traceJit.capacity = lt.capacity;
                TraceResult result = executeTrace(traceJit, frame->regs, &state);
                if (result.completed) {
                    frame->ip = &frame->chunk->code[loopPc + 1];
                    break;
                }
                if (lt.sunkAllocs) {
                    for (auto& sunk : *lt.sunkAllocs) {
                        BblTable* tbl = state.allocTable();
                        for (auto& f : sunk.fields)
                            tbl->set(BblValue::makeString(state.intern(f.name)), frame->regs[f.srcReg]);
                        frame->regs[sunk.destReg] = BblValue::makeTable(tbl);
                    }
                }
            }

            if (!state.gcPaused && state.allocCount >= state.gen0Threshold) state.gcMinor();
            if (state.maxSteps && ++state.stepCount > state.maxSteps)
                throw BBL::Error{"step limit exceeded"};
            if (state.terminated.load(std::memory_order_relaxed))
                throw BblTerminated{};
            break;
        }

        case OP_AND: if (isFalsy(R(A))) frame->ip += sBx; break;
        case OP_OR:  if (!isFalsy(R(A))) frame->ip += sBx; break;

        case OP_CLOSURE: {
            BblClosure* proto = K(Bx).closureVal();
            BblClosure* closure = state.closureSlab.alloc();
            closure->chunk = proto->chunk;
            closure->arity = proto->arity;
            closure->name = proto->name;
            closure->captureDescs = proto->captureDescs;
            closure->captures.resize(proto->captureDescs.size());
            closure->gcNext = state.nurseryHead; state.nurseryHead = closure;
            state.allocCount++;

            for (size_t i = 0; i < proto->captureDescs.size(); i++) {
                auto& desc = proto->captureDescs[i];
                if (desc.srcType == 0)
                    closure->captures[i] = R(desc.srcIdx);
                else if (R(0).isClosure())
                    closure->captures[i] = R(0).closureVal()->captures[desc.srcIdx];
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
            if (callee.type() != BBL::Type::Fn || callee.isCFn() || !callee.isClosure())
                throw BBL::Error{"tail call target must be a bytecode function"};
            BblClosure* closure = callee.closureVal();
            if (closure->arity != B)
                throw BBL::Error{"wrong number of arguments"};
            // Copy args to current frame
            R(0) = callee;
            for (int i = 0; i < B; i++)
                R(1 + i) = frame->regs[A + 1 + i];
            frame->chunk = &closure->chunk;
            frame->ip = closure->chunk.code.data();
            state.vm->stackTop = frame->regs + closure->chunk.numRegs;
            break;
        }

        case OP_RETURN: {
            BblValue result = R(A);
            uint8_t returnDest = static_cast<uint8_t>(frame->returnDest);
            state.vm->frameCount--;
            if (state.vm->frameCount == 0) {
                state.vm->stackTop = state.vm->stack.data();
                state.vm->stack[0] = result;
                goto done;
            }
            frame = &state.vm->frames[state.vm->frameCount - 1];
            state.vm->stackTop = frame->regs + frame->chunk->numRegs;
            R(returnDest) = result;
            break;
        }

        case OP_VECTOR: {
            // A=dest, B=argc, C=typeNameConstIdx
            std::string elemType = K(C).stringVal()->data;
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
            std::string tname = K(C).stringVal()->data;
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
            if (arg.type() == BBL::Type::Vector) R(A) = BblValue::makeBinary(state.allocBinary(arg.vectorVal()->data));
            else if (arg.type() == BBL::Type::Struct) R(A) = BblValue::makeBinary(state.allocBinary(arg.structVal()->data));
            else if (arg.type() == BBL::Type::Int) {
                if (arg.intVal() < 0) throw BBL::Error{"binary: size must be non-negative"};
                R(A) = BblValue::makeBinary(state.allocBinary(std::vector<uint8_t>(static_cast<size_t>(arg.intVal()), 0)));
            } else throw BBL::Error{"binary: invalid argument type"};
            break;
        }

        case OP_GETFIELD: {
            // A=dest, B=obj, C=fieldNameConstIdx
            BblValue& obj = R(B);
            std::string fieldName = K(C).stringVal()->data;
            if (obj.type() == BBL::Type::Struct) {
                for (auto& fd : obj.structVal()->desc->fields) {
                    if (fd.name == fieldName) { R(A) = state.readField(obj.structVal(), fd); goto field_ok; }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type() == BBL::Type::Table) {
                auto res = obj.tableVal()->get(BblValue::makeString(state.intern(fieldName)));
                R(A) = res.value_or(BblValue::makeNull());
            } else throw BBL::Error{"cannot access field on " + std::string(::typeName(obj.type()))};
            field_ok: break;
        }

        case OP_SETFIELD: {
            // A=val, B=obj, C=fieldNameConstIdx
            BblValue& obj = R(B);
            std::string fieldName = K(C).stringVal()->data;
            if (obj.type() == BBL::Type::Struct) {
                for (auto& fd : obj.structVal()->desc->fields) {
                    if (fd.name == fieldName) { state.writeField(obj.structVal(), fd, R(A)); goto setf_ok; }
                }
                throw BBL::Error{"struct has no field '" + fieldName + "'"};
            } else if (obj.type() == BBL::Type::Table) {
                obj.tableVal()->set(BblValue::makeString(state.intern(fieldName)), R(A));
            } else throw BBL::Error{"cannot set field"};
            setf_ok: break;
        }

        case OP_GETINDEX: {
            BblValue& obj = R(B); BblValue& idx = R(C);
            if (obj.type() == BBL::Type::Vector) {
                if (idx.type() != BBL::Type::Int) throw BBL::Error{"vector index must be int"};
                R(A) = state.readVecElem(obj.vectorVal(), static_cast<size_t>(idx.intVal()));
            } else if (obj.type() == BBL::Type::Table) {
                R(A) = obj.tableVal()->get(idx).value_or(BblValue::makeNull());
            } else if (obj.type() == BBL::Type::Binary) {
                size_t i = static_cast<size_t>(idx.intVal());
                if (i >= obj.binaryVal()->length()) throw BBL::Error{"binary index out of range"};
                R(A) = BblValue::makeInt(obj.binaryVal()->data[i]);
            } else throw BBL::Error{"cannot index " + std::string(::typeName(obj.type()))};
            break;
        }

        case OP_SETINDEX: {
            // A=val, B=obj, C=idx
            BblValue& obj = R(B); BblValue& idx = R(C);
            if (obj.type() == BBL::Type::Vector) state.writeVecElem(obj.vectorVal(), static_cast<size_t>(idx.intVal()), R(A));
            else if (obj.type() == BBL::Type::Table) obj.tableVal()->set(idx, R(A));
            else throw BBL::Error{"cannot set index"};
            break;
        }

        case OP_MCALL: {
            // A=receiver(+dest), B=argc, C=methodNameConstIdx
            BblString* methodStr = K(C).stringVal();
            BblValue receiver = R(A);
            BblValue argsBuf[8];
            int nargs = B;
            for (int i = 0; i < nargs; i++) argsBuf[i] = R(A + 1 + i);

            if (receiver.type() == BBL::Type::Vector) {
                BblVec* vec = receiver.vectorVal();
                if (methodStr == state.m.length) R(A) = BblValue::makeInt(static_cast<int64_t>(vec->length()));
                else if (methodStr == state.m.push) { for (int i = 0; i < nargs; i++) state.packValue(vec, argsBuf[i]); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.pop) {
                    if (vec->length() == 0) throw BBL::Error{"pop on empty vector"};
                    R(A) = state.readVecElem(vec, vec->length() - 1);
                    vec->data.resize(vec->data.size() - vec->elemSize);
                } else if (methodStr == state.m.clear) { vec->data.clear(); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.at) R(A) = state.readVecElem(vec, static_cast<size_t>(argsBuf[0].intVal()));
                else if (methodStr == state.m.set) { state.writeVecElem(vec, static_cast<size_t>(argsBuf[0].intVal()), argsBuf[1]); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.resize) { vec->data.resize(static_cast<size_t>(argsBuf[0].intVal()) * vec->elemSize, 0); R(A) = BblValue::makeNull(); }
                else throw BBL::Error{"unknown vector method: " + methodStr->data};
            } else if (receiver.type() == BBL::Type::String) {
                BblString* str = receiver.stringVal();
                if (methodStr == state.m.length) R(A) = BblValue::makeInt(static_cast<int64_t>(str->data.size()));
                else if (methodStr == state.m.at) { R(A) = BblValue::makeString(state.intern(std::string(1, str->data[static_cast<size_t>(argsBuf[0].intVal())]))); }
                else if (methodStr == state.m.slice) {
                    int64_t start = argsBuf[0].intVal();
                    int64_t len = static_cast<size_t>(nargs) > 1 ? argsBuf[1].intVal() : static_cast<int64_t>(str->data.size()) - start;
                    R(A) = BblValue::makeString(state.intern(str->data.substr(static_cast<size_t>(start), static_cast<size_t>(len))));
                } else if (methodStr == state.m.find) {
                    size_t start = nargs > 1 ? static_cast<size_t>(argsBuf[1].intVal()) : 0;
                    auto pos = str->data.find(argsBuf[0].stringVal()->data, start);
                    R(A) = BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
                } else if (methodStr == state.m.contains) R(A) = BblValue::makeBool(str->data.find(argsBuf[0].stringVal()->data) != std::string::npos);
                else if (methodStr == state.m.starts_with) R(A) = BblValue::makeBool(str->data.starts_with(argsBuf[0].stringVal()->data));
                else if (methodStr == state.m.ends_with) R(A) = BblValue::makeBool(str->data.ends_with(argsBuf[0].stringVal()->data));
                else if (methodStr == state.m.upper) { std::string r = str->data; for (auto& c : r) c = static_cast<char>(toupper(c)); R(A) = BblValue::makeString(state.intern(r)); }
                else if (methodStr == state.m.lower) { std::string r = str->data; for (auto& c : r) c = static_cast<char>(tolower(c)); R(A) = BblValue::makeString(state.intern(r)); }
                else if (methodStr == state.m.trim) {
                    auto s = str->data.find_first_not_of(" \t\n\r");
                    auto e = str->data.find_last_not_of(" \t\n\r");
                    R(A) = BblValue::makeString(state.intern(s == std::string::npos ? "" : str->data.substr(s, e - s + 1)));
                } else if (methodStr == state.m.replace) {
                    std::string result = str->data, from = argsBuf[0].stringVal()->data, to = argsBuf[1].stringVal()->data;
                    size_t pos = 0;
                    while ((pos = result.find(from, pos)) != std::string::npos) { result.replace(pos, from.size(), to); pos += to.size(); }
                    R(A) = BblValue::makeString(state.intern(result));
                } else if (methodStr == state.m.split) {
                    std::string delim = argsBuf[0].stringVal()->data;
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
                } else throw BBL::Error{"unknown string method: " + methodStr->data};
            } else if (receiver.type() == BBL::Type::Binary) {
                BblBinary* bin = receiver.binaryVal();
                if (methodStr == state.m.length) R(A) = BblValue::makeInt(static_cast<int64_t>(bin->length()));
                else if (methodStr == state.m.at) { bin->materialize(); R(A) = BblValue::makeInt(bin->data[static_cast<size_t>(argsBuf[0].intVal())]); }
                else if (methodStr == state.m.set) { bin->materialize(); bin->data[static_cast<size_t>(argsBuf[0].intVal())] = static_cast<uint8_t>(argsBuf[1].intVal()); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.slice) {
                    bin->materialize();
                    size_t s = static_cast<size_t>(argsBuf[0].intVal()), l = static_cast<size_t>(argsBuf[1].intVal());
                    R(A) = BblValue::makeBinary(state.allocBinary(std::vector<uint8_t>(bin->data.begin() + s, bin->data.begin() + s + l)));
                } else if (methodStr == state.m.resize) { bin->materialize(); bin->data.resize(static_cast<size_t>(argsBuf[0].intVal()), 0); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.copy_from) {
                    bin->materialize();
                    BblBinary* src = argsBuf[0].binaryVal(); src->materialize();
                    size_t dO = static_cast<size_t>(nargs) > 1 ? static_cast<size_t>(argsBuf[1].intVal()) : 0;
                    size_t sO = static_cast<size_t>(nargs) > 2 ? static_cast<size_t>(argsBuf[2].intVal()) : 0;
                    size_t ln = static_cast<size_t>(nargs) > 3 ? static_cast<size_t>(argsBuf[3].intVal()) : src->length() - sO;
                    std::memcpy(bin->data.data() + dO, src->data.data() + sO, ln);
                    R(A) = BblValue::makeNull();
                } else throw BBL::Error{"unknown binary method: " + methodStr->data};
            } else if (receiver.type() == BBL::Type::Table) {
                BblTable* tbl = receiver.tableVal();
                if (methodStr == state.m.length) R(A) = BblValue::makeInt(static_cast<int64_t>(tbl->length()));
                else if (methodStr == state.m.get) R(A) = tbl->get(argsBuf[0]).value_or(static_cast<size_t>(nargs) > 1 ? argsBuf[1] : BblValue::makeNull());
                else if (methodStr == state.m.set) { tbl->set(argsBuf[0], argsBuf[1]); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.del) { tbl->del(argsBuf[0]); R(A) = BblValue::makeNull(); }
                else if (methodStr == state.m.has) R(A) = BblValue::makeBool(tbl->has(argsBuf[0]));
                else if (methodStr == state.m.keys) {
                    BblTable* keys = state.allocTable(); int64_t i = 0;
                    tbl->ensureOrder();
                    if (tbl->order) for (auto& k : *tbl->order) keys->set(BblValue::makeInt(i++), k);
                    R(A) = BblValue::makeTable(keys);
                } else if (methodStr == state.m.push) {
                    for (int i = 0; i < nargs; i++) { tbl->set(BblValue::makeInt(tbl->nextIntKey), argsBuf[i]); }
                    R(A) = BblValue::makeNull();
                } else if (methodStr == state.m.pop) {
                    bool found = false;
                    tbl->ensureOrder();
                    for (auto it = tbl->order->rbegin(); it != tbl->order->rend(); ++it) {
                        if (it->type() == BBL::Type::Int) {
                            R(A) = tbl->get(*it).value_or(BblValue::makeNull());
                            tbl->del(*it);
                            found = true;
                            break;
                        }
                    }
                    if (!found) throw BBL::Error{"pop: no integer keys"};
                } else if (methodStr == state.m.at) {
                    size_t idx = static_cast<size_t>(argsBuf[0].intVal());
                    tbl->ensureOrder();
                    if (idx >= tbl->order->size()) throw BBL::Error{"table index out of range"};
                    R(A) = tbl->get((*tbl->order)[idx]).value_or(BblValue::makeNull());
                } else {
                    auto val = tbl->get(BblValue::makeString(const_cast<BblString*>(methodStr)));
                    if (val.has_value() && val->type() == BBL::Type::Fn) {
                        BblValue fn = val.value();
                        R(A + 1) = BblValue::makeTable(tbl);
                        for (int i = 0; i < nargs; i++) R(A + 2 + i) = argsBuf[i];
                        R(A) = fn;
                        if (!callValue(state, frame, A, nargs + 1, A))
                            throw BBL::Error{"table method not callable"};
                    } else {
                        throw BBL::Error{"unknown table method: " + methodStr->data};
                    }
                }
            } else if (receiver.type() == BBL::Type::UserData) {
                auto it = receiver.userdataVal()->desc->methods.find(methodStr->data);
                if (it == receiver.userdataVal()->desc->methods.end())
                    throw BBL::Error{"unknown method '" + methodStr->data + "'"};
                state.callArgs.clear();
                state.callArgs.push_back(receiver);
                for (int i = 0; i < nargs; i++) state.callArgs.push_back(argsBuf[i]);
                state.hasReturn = false; state.returnValue = BblValue::makeNull();
                it->second(&state);
                R(A) = state.hasReturn ? state.returnValue : BblValue::makeNull();
            } else throw BBL::Error{"cannot call method on " + std::string(::typeName(receiver.type()))};
            break;
        }

        case OP_LENGTH: {
            BblValue& obj = R(B);
            if (obj.type() == BBL::Type::Vector) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.vectorVal()->length()));
            else if (obj.type() == BBL::Type::String) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.stringVal()->data.size()));
            else if (obj.type() == BBL::Type::Binary) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.binaryVal()->length()));
            else if (obj.type() == BBL::Type::Table) R(A) = BblValue::makeInt(static_cast<int64_t>(obj.tableVal()->length()));
            else throw BBL::Error{"cannot get length"};
            break;
        }

        case OP_SIZEOF: {
            std::string tname = K(B).stringVal()->data;
            auto dit = state.structDescs.find(tname);
            if (dit == state.structDescs.end()) throw BBL::Error{"unknown struct type: " + tname};
            R(A) = BblValue::makeInt(static_cast<int64_t>(dit->second.totalSize));
            break;
        }

        case OP_EXEC: {
            if (R(B).type() != BBL::Type::String) throw BBL::Error{"exec: argument must be string"};
            BblLexer lexer(R(B).stringVal()->data.c_str());
            auto nodes = parse(lexer);
            R(A) = state.execExpr(R(B).stringVal()->data);
            break;
        }

        case OP_EXECFILE: {
            if (R(B).type() != BBL::Type::String) throw BBL::Error{"execfile: argument must be string"};
            state.execfile(R(B).stringVal()->data);
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
    if (callee.type() == BBL::Type::Fn) {
        if (callee.isCFn()) {
            state.callArgs.clear();
            for (int i = 0; i < argc; i++)
                state.callArgs.push_back(frame->regs[base + 1 + i]);
            state.hasReturn = false; state.returnValue = BblValue::makeNull();
            callee.cfnVal()(&state);
            frame->regs[destInCaller] = state.hasReturn ? state.returnValue : BblValue::makeNull();
            return true;
        }
        if (callee.isClosure()) {
            BblClosure* closure = callee.closureVal();
            if (closure->arity != argc)
                throw BBL::Error{"wrong number of arguments: expected " +
                    std::to_string(closure->arity) + " got " + std::to_string(argc)};
            if (state.vm->frameCount >= VM_MAX_FRAMES)
                throw BBL::Error{"stack overflow"};

            CallFrame* newFrame = &state.vm->frames[state.vm->frameCount++];
            newFrame->chunk = &closure->chunk;
            newFrame->ip = closure->chunk.code.data();
            newFrame->regs = &frame->regs[base];
            newFrame->returnDest = destInCaller;

            BblValue* needed = newFrame->regs + closure->chunk.numRegs;
            if (needed > state.vm->stackTop) state.vm->stackTop = needed;

            frame = newFrame;
            return true;
        }
        throw BBL::Error{"raw BblFn calls not supported in JIT mode"};
    }
    return false;
}
