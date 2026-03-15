#include "compiler.h"
#include "bbl.h"
#include "vm.h"
#include <unordered_set>

namespace bbl {


int CompilerState::resolveCapture(uint32_t symbolId) {
    if (!enclosing) return -1;
    int local = enclosing->resolveLocal(symbolId);
    if (local != -1) {
        for (size_t i = 0; i < captures.size(); i++)
            if (captures[i].srcType == 0 && captures[i].srcIdx == static_cast<uint8_t>(local))
                return static_cast<int>(i);
        if (captures.size() >= 255) throw Error{"too many captures (limit 255)"};
        captures.push_back({0, static_cast<uint8_t>(local)});
        return static_cast<int>(captures.size() - 1);
    }
    int cap = enclosing->resolveCapture(symbolId);
    if (cap != -1) {
        for (size_t i = 0; i < captures.size(); i++)
            if (captures[i].srcType == 1 && captures[i].srcIdx == static_cast<uint8_t>(cap))
                return static_cast<int>(i);
        if (captures.size() >= 255) throw Error{"too many captures (limit 255)"};
        captures.push_back({1, static_cast<uint8_t>(cap)});
        return static_cast<int>(captures.size() - 1);
    }
    return -1;
}

static uint8_t compileExpr(BblState& state, CompilerState& cs, const AstNode& node, uint8_t dest);
static void compileInto(BblState& state, CompilerState& cs, const AstNode& node, uint8_t dest) {
    uint8_t r = compileExpr(state, cs, node, dest);
    if (r != dest) cs.chunk.emitABC(OP_MOVE, dest, r, 0, node.line);
}
static uint8_t compileList(BblState& state, CompilerState& cs, const AstNode& node, uint8_t dest);
static void compileFn(BblState& state, CompilerState& cs, const AstNode& node, const std::string& assignName, uint8_t dest);

static uint8_t addSymConst(BblState& state, CompilerState& cs, uint32_t symId) {
    size_t idx = cs.chunk.addConstant(BblValue::makeInt(static_cast<int64_t>(symId)));
    if (idx > 255) throw Error{"too many field/symbol constants (limit 256 per function)"};
    return static_cast<uint8_t>(idx);
}

static uint8_t addStrConst(BblState& state, CompilerState& cs, const std::string& s) {
    size_t idx = cs.chunk.addConstant(BblValue::makeString(state.intern(s)));
    if (idx > 255) throw Error{"too many string constants (limit 256 per function)"};
    return static_cast<uint8_t>(idx);
}

static int emitJump(CompilerState& cs, uint8_t op, uint8_t A, int line) {
    int offset = static_cast<int>(cs.chunk.code.size());
    cs.chunk.emitAsBx(op, A, 0, line);
    return offset;
}

static uint16_t addConstIdx(CompilerState& cs, const BblValue& val) {
    size_t idx = cs.chunk.addConstant(val);
    if (idx > 65535) throw Error{"too many constants (limit 65536 per function)"};
    return static_cast<uint16_t>(idx);
}

static uint8_t checkArgc(size_t count, int line) {
    if (count > 255) throw Error{"too many arguments (limit 255) at line " + std::to_string(line)};
    return static_cast<uint8_t>(count);
}

static void patchJump(CompilerState& cs, int offset) {
    int jump = static_cast<int>(cs.chunk.code.size()) - offset - 1;
    if (jump < -32767 || jump > 32767)
        throw Error{"function too large (jump distance exceeds limit)"};
    cs.chunk.patchsBx(offset, jump);
}

static uint8_t compileExpr(BblState& state, CompilerState& cs, const AstNode& node, uint8_t dest) {
    if (++cs.compileDepth > 512) throw Error{"compilation error: nesting too deep (limit 512)"};
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{cs.compileDepth};
    switch (node.type) {
        case NodeType::IntLiteral: {
            int64_t v = node.intVal;
            if (v >= -32768 && v <= 32767) {
                cs.chunk.emitAsBx(OP_LOADINT, dest, static_cast<int>(v), node.line);
            } else {
                uint16_t idx = addConstIdx(cs, BblValue::makeInt(v));
                cs.chunk.emitABx(OP_LOADK, dest, idx, node.line);
            }
            return dest;
        }
        case NodeType::FloatLiteral: {
            uint16_t idx = addConstIdx(cs, BblValue::makeFloat(node.floatVal));
            cs.chunk.emitABx(OP_LOADK, dest, idx, node.line);
            return dest;
        }
        case NodeType::StringLiteral: {
            uint16_t idx = addConstIdx(cs, BblValue::makeString(state.intern(node.stringVal)));
            cs.chunk.emitABx(OP_LOADK, dest, idx, node.line);
            return dest;
        }
        case NodeType::BoolLiteral:
            cs.chunk.emitABC(OP_LOADBOOL, dest, node.boolVal ? 1 : 0, 0, node.line);
            return dest;
        case NodeType::NullLiteral:
            cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
            return dest;
        case NodeType::BinaryLiteral: {
            BblBinary* bin = node.binarySource
                ? state.allocLazyBinary(node.binarySource, node.binarySize, node.isCompressed)
                : state.allocBinary(node.binaryData);
            uint16_t idx = addConstIdx(cs, BblValue::makeBinary(bin));
            cs.chunk.emitABx(OP_LOADK, dest, idx, node.line);
            return dest;
        }
        case NodeType::Symbol: {
            uint32_t symId = state.resolveSymbol(node.stringVal);
            int lr = cs.resolveLocal(symId);
            if (lr != -1) return static_cast<uint8_t>(lr);
            int cap = cs.resolveCapture(symId);
            if (cap != -1) {
                cs.chunk.emitABC(OP_GETCAPTURE, dest, static_cast<uint8_t>(cap), 0, node.line);
                return dest;
            }
            uint16_t kidx = addConstIdx(cs, BblValue::makeInt(static_cast<int64_t>(symId)));
            cs.chunk.emitABx(state.currentEnv ? OP_ENVGET : OP_GETGLOBAL, dest, kidx, node.line);
            return dest;
        }
        case NodeType::List:
            return compileList(state, cs, node, dest);
        case NodeType::DotAccess: {
            uint8_t objReg = compileExpr(state, cs, node.children[0], dest);
            if (objReg != dest) {
                cs.chunk.emitABC(OP_MOVE, dest, objReg, 0, node.line);
                objReg = dest;
            }
            if (!node.stringVal.empty()) {
                uint8_t nameIdx = addStrConst(state, cs, node.stringVal);
                cs.chunk.emitABC(OP_GETFIELD, dest, objReg, nameIdx, node.line);
            } else {
                uint8_t idxReg = cs.allocReg();
                cs.chunk.emitAsBx(OP_LOADINT, idxReg, static_cast<int>(node.intVal), node.line);
                cs.chunk.emitABC(OP_GETINDEX, dest, objReg, idxReg, node.line);
                cs.freeReg();
            }
            return dest;
        }
        case NodeType::ColonAccess:
            throw Error{"colon access must be called as a method at line " + std::to_string(node.line)};
    }
    return dest;
}

static OpCode arithOp(const std::string& name) {
    if (name == "+") return OP_ADD;
    if (name == "-") return OP_SUB;
    if (name == "*") return OP_MUL;
    if (name == "/") return OP_DIV;
    if (name == "%") return OP_MOD;
    if (name == "band") return OP_BAND;
    if (name == "bor") return OP_BOR;
    if (name == "bxor") return OP_BXOR;
    return OP_LOADNULL;
}

static OpCode cmpOp(const std::string& name) {
    if (name == "==") return OP_EQ;
    if (name == "!=") return OP_NEQ;
    if (name == "<") return OP_LT;
    if (name == ">") return OP_GT;
    if (name == "<=") return OP_LTE;
    if (name == ">=") return OP_GTE;
    return OP_LOADNULL;
}

static uint8_t compileList(BblState& state, CompilerState& cs, const AstNode& node, uint8_t dest) {
    if (node.children.empty()) {
        cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
        return dest;
    }
    auto& head = node.children[0];

    // Method call: (obj:method args...)
    if (head.type == NodeType::ColonAccess) {
        uint8_t base = dest;
        uint8_t savedNext = cs.nextReg;
        if (base < cs.nextReg) base = cs.allocReg();
        cs.freeRegsTo(base);

        compileInto(state, cs, head.children[0], base);
        if (cs.nextReg <= base) cs.nextReg = base + 1;
        uint8_t argc = checkArgc(node.children.size() - 1, node.line);
        for (size_t i = 1; i < node.children.size(); i++) {
            uint8_t argReg = cs.allocReg();
            compileInto(state, cs, node.children[i], argReg);
        }
        uint8_t nameIdx = addStrConst(state, cs, head.stringVal);
        cs.chunk.emitABC(OP_MCALL, base, argc, nameIdx, node.line);
        cs.freeRegsTo(savedNext);
        if (base != dest) {
            cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
        }
        return dest;
    }

    if (head.type != NodeType::Symbol) {
        uint8_t base = dest;
        uint8_t savedNext = cs.nextReg;
        if (base < cs.nextReg) base = cs.allocReg();
        cs.freeRegsTo(base);

        compileInto(state, cs, head, base);
        if (cs.nextReg <= base) cs.nextReg = base + 1;
        uint8_t argc = checkArgc(node.children.size() - 1, node.line);
        for (size_t i = 1; i < node.children.size(); i++) {
            uint8_t argReg = cs.allocReg();
            compileInto(state, cs, node.children[i], argReg);
        }
        cs.chunk.emitABC(OP_CALL, base, argc, 1, node.line);
        cs.freeRegsTo(savedNext);
        if (base != dest) cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
        return dest;
    }

    const std::string& op = head.stringVal;

    // Arithmetic: chained binary ops
    OpCode aop = arithOp(op);
    if (aop != OP_LOADNULL) {
        if (node.children.size() < 3) throw Error{"'" + op + "' requires at least 2 arguments at line " + std::to_string(node.line)};
        uint8_t savedNext = cs.nextReg;
        uint8_t regA = compileExpr(state, cs, node.children[1], dest);
        for (size_t i = 2; i < node.children.size(); i++) {
            bool emittedSpecial = false;
            if (node.children[i].type == NodeType::IntLiteral && node.children.size() == 3) {
                int64_t imm = node.children[i].intVal;
                if (imm >= -32768 && imm <= 32767 && regA == dest) {
                    if (aop == OP_ADD) {
                        cs.chunk.emitAsBx(OP_ADDI, regA, static_cast<int>(imm), node.line);
                        emittedSpecial = true;
                    } else if (aop == OP_SUB) {
                        cs.chunk.emitAsBx(OP_SUBI, regA, static_cast<int>(imm), node.line);
                        emittedSpecial = true;
                    }
                }
            }
            if (!emittedSpecial) {
                uint8_t regB = cs.allocReg();
                uint8_t actualB = compileExpr(state, cs, node.children[i], regB);
                cs.chunk.emitABC(aop, dest, regA, actualB, node.line);
            }
            regA = dest;
            cs.freeRegsTo(savedNext > dest + 1 ? savedNext : dest + 1);
        }
        return dest;
    }

    // Comparisons
    OpCode cop = cmpOp(op);
    if (cop != OP_LOADNULL) {
        if (node.children.size() != 3) throw Error{"'" + op + "' requires exactly 2 arguments at line " + std::to_string(node.line)};
        uint8_t savedNext = cs.nextReg;
        uint8_t regA = compileExpr(state, cs, node.children[1], cs.allocReg());
        uint8_t regB = compileExpr(state, cs, node.children[2], cs.allocReg());
        cs.chunk.emitABC(cop, dest, regA, regB, node.line);
        cs.freeRegsTo(savedNext);
        return dest;
    }

    if (op == "shl" || op == "shr") {
        if (node.children.size() != 3) throw Error{"'" + op + "' requires exactly 2 arguments at line " + std::to_string(node.line)};
        uint8_t savedNext = cs.nextReg;
        uint8_t regA = compileExpr(state, cs, node.children[1], cs.allocReg());
        uint8_t regB = compileExpr(state, cs, node.children[2], cs.allocReg());
        cs.chunk.emitABC(op == "shl" ? OP_SHL : OP_SHR, dest, regA, regB, node.line);
        cs.freeRegsTo(savedNext);
        return dest;
    }

    if (op == "bnot") {
        if (node.children.size() < 2) throw Error{"'bnot' requires an argument at line " + std::to_string(node.line)};
        uint8_t regA = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_BNOT, dest, regA, 0, node.line);
        return dest;
    }

    if (op == "not") {
        if (node.children.size() < 2) throw Error{"'not' requires an argument at line " + std::to_string(node.line)};
        uint8_t regA = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_NOT, dest, regA, 0, node.line);
        return dest;
    }

    // Assignment
    if (op == "=") {
        if (node.children.size() < 3) throw Error{"'=' requires at least 2 arguments at line " + std::to_string(node.line)};
        auto& target = node.children[1];

        if (target.type == NodeType::Symbol) {
            uint32_t symId = state.resolveSymbol(target.stringVal);
            int localReg = cs.resolveLocal(symId);

            bool isFnDef = node.children[2].type == NodeType::List &&
                           !node.children[2].children.empty() &&
                           node.children[2].children[0].type == NodeType::Symbol &&
                           node.children[2].children[0].stringVal == "fn";

            // Check if fn is small enough to inline at call sites
            if (isFnDef) {
                auto& fnNode = node.children[2];
                // Inline if: has param list, body is 1 expression, no nested fn
                if (fnNode.children.size() == 3 && fnNode.children[1].type == NodeType::List) {
                    cs.inlinableFns[symId] = &node.children[2];
                }
            }

            if (localReg == -1) {
                int cap = cs.resolveCapture(symId);
                if (cap != -1) {
                    if (isFnDef) compileFn(state, cs, node.children[2], target.stringVal, dest);
                    else compileExpr(state, cs, node.children[2], dest);
                    cs.chunk.emitABC(OP_SETCAPTURE, dest, static_cast<uint8_t>(cap), 0, node.line);
                    return dest;
                }
                // New local or global
                if (cs.scopeDepth > 0 || cs.enclosing) {
                    uint8_t reg = cs.allocReg();
                    cs.localRegs[symId] = reg;
                    if (isFnDef) compileFn(state, cs, node.children[2], target.stringVal, reg);
                    else compileInto(state, cs, node.children[2], reg);
                    return reg;
                }
                // Top-level global
                if (isFnDef) compileFn(state, cs, node.children[2], target.stringVal, dest);
                else compileExpr(state, cs, node.children[2], dest);
                uint16_t kidx = addConstIdx(cs, BblValue::makeInt(static_cast<int64_t>(symId)));
                cs.chunk.emitABx(state.currentEnv ? OP_ENVSET : OP_SETGLOBAL, dest, kidx, node.line);
                return dest;
            }
            // Existing local
            if (isFnDef) compileFn(state, cs, node.children[2], target.stringVal, static_cast<uint8_t>(localReg));
            else compileInto(state, cs, node.children[2], static_cast<uint8_t>(localReg));
            return static_cast<uint8_t>(localReg);
        } else if (target.type == NodeType::DotAccess) {
            uint8_t savedNext = cs.nextReg;
            uint8_t valReg = compileExpr(state, cs, node.children[2], cs.allocReg());
            uint8_t objReg = compileExpr(state, cs, target.children[0], cs.allocReg());
            if (!target.stringVal.empty()) {
                uint8_t nameIdx = addStrConst(state, cs, target.stringVal);
                cs.chunk.emitABC(OP_SETFIELD, valReg, objReg, nameIdx, node.line);
            } else {
                uint8_t idxReg = cs.allocReg();
                cs.chunk.emitAsBx(OP_LOADINT, idxReg, static_cast<int>(target.intVal), node.line);
                cs.chunk.emitABC(OP_SETINDEX, valReg, objReg, idxReg, node.line);
            }
            if (dest != valReg) cs.chunk.emitABC(OP_MOVE, dest, valReg, 0, node.line);
            cs.freeRegsTo(savedNext);
            return dest;
        } else {
            throw Error{"invalid assignment target at line " + std::to_string(node.line)};
        }
    }

    // (defn name (args) body) → compile as (= name (fn (args) body))
    if (op == "defn") {
        if (node.children.size() < 4) throw Error{"'defn' requires name, params, and body at line " + std::to_string(node.line)};
        auto& target = node.children[1];
        if (target.type != NodeType::Symbol) throw Error{"'defn' name must be a symbol at line " + std::to_string(node.line)};
        uint32_t symId = state.resolveSymbol(target.stringVal);

        AstNode fnNode;
        fnNode.type = NodeType::List;
        fnNode.line = node.line;
        AstNode fnSym; fnSym.type = NodeType::Symbol; fnSym.stringVal = "fn"; fnSym.line = node.line;
        fnNode.children.push_back(std::move(fnSym));
        for (size_t i = 2; i < node.children.size(); i++)
            fnNode.children.push_back(node.children[i]);

        int localReg = cs.resolveLocal(symId);
        if (localReg == -1) {
            if (cs.scopeDepth > 0 || cs.enclosing) {
                uint8_t reg = cs.allocReg();
                cs.localRegs[symId] = reg;
                compileFn(state, cs, fnNode, target.stringVal, reg);
                return reg;
            }
            compileFn(state, cs, fnNode, target.stringVal, dest);
            uint16_t kidx = addConstIdx(cs, BblValue::makeInt(static_cast<int64_t>(symId)));
            cs.chunk.emitABx(state.currentEnv ? OP_ENVSET : OP_SETGLOBAL, dest, kidx, node.line);            return dest;
        }
        compileFn(state, cs, fnNode, target.stringVal, static_cast<uint8_t>(localReg));
        return static_cast<uint8_t>(localReg);
    }

    // Control flow
    if (op == "if") {
        if (node.children.size() < 3) throw Error{"'if' requires condition and body at line " + std::to_string(node.line)};
        auto& cond = node.children[1];
        bool fusedCmp = false;
        if (cond.type == NodeType::List && cond.children.size() == 3 &&
            cond.children[0].type == NodeType::Symbol) {
            const std::string& cop = cond.children[0].stringVal;
            OpCode fop = OP_LOADNULL;
            if (cop == "<") fop = OP_LTJMP;
            else if (cop == "<=") fop = OP_LEJMP;
            else if (cop == ">") fop = OP_GTJMP;
            else if (cop == ">=") fop = OP_GEJMP;
            if (fop != OP_LOADNULL) {
                uint8_t saveReg = cs.nextReg;
                uint8_t rA = compileExpr(state, cs, cond.children[1], cs.allocReg());
                uint8_t rB = compileExpr(state, cs, cond.children[2], cs.allocReg());
                cs.chunk.emitABC(fop, rA, rB, 0, node.line);
                int elseJump = emitJump(cs, OP_JMP, 0, node.line);
                cs.freeRegsTo(saveReg);
                compileInto(state, cs, node.children[2], dest);
                if (node.children.size() > 3) {
                    int endJump = emitJump(cs, OP_JMP, 0, node.line);
                    patchJump(cs, elseJump);
                    compileInto(state, cs, node.children[3], dest);
                    patchJump(cs, endJump);
                } else {
                    int endJump = emitJump(cs, OP_JMP, 0, node.line);
                    patchJump(cs, elseJump);
                    cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
                    patchJump(cs, endJump);
                }
                fusedCmp = true;
                return dest;
            }
        }
        if (!fusedCmp) {
        uint8_t condReg = compileExpr(state, cs, node.children[1], dest);
        int elseJump = emitJump(cs, OP_JMPFALSE, condReg, node.line);
        compileInto(state, cs, node.children[2], dest);
        if (node.children.size() > 3) {
            int endJump = emitJump(cs, OP_JMP, 0, node.line);
            patchJump(cs, elseJump);
            compileInto(state, cs, node.children[3], dest);
            patchJump(cs, endJump);
        } else {
            int endJump = emitJump(cs, OP_JMP, 0, node.line);
            patchJump(cs, elseJump);
            cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
            patchJump(cs, endJump);
        }
        }
        return dest;
    }

    if (op == "loop") {
        if (node.children.size() < 2) throw Error{"'loop' requires a condition at line " + std::to_string(node.line)};
        CompilerState::LoopInfo loopInfo;
        loopInfo.start = static_cast<int>(cs.chunk.code.size());
        cs.loops.push_back(loopInfo);

        auto& cond = node.children[1];
        bool fusedCmp = false;
        if (cond.type == NodeType::List && cond.children.size() == 3 &&
            cond.children[0].type == NodeType::Symbol) {
            const std::string& cop = cond.children[0].stringVal;
            OpCode fop = OP_LOADNULL;
            if (cop == "<") fop = OP_LTJMP;
            else if (cop == "<=") fop = OP_LEJMP;
            else if (cop == ">") fop = OP_GTJMP;
            else if (cop == ">=") fop = OP_GEJMP;
            if (fop != OP_LOADNULL) {
                uint8_t rA = compileExpr(state, cs, cond.children[1], cs.allocReg());
                uint8_t rB = compileExpr(state, cs, cond.children[2], cs.allocReg());
                cs.chunk.emitABC(fop, rA, rB, 0, node.line);
                int exitJump = emitJump(cs, OP_JMP, 0, node.line);
                cs.freeRegsTo(loopInfo.start == static_cast<int>(cs.chunk.code.size()) ? cs.nextReg : cs.nextReg);

                for (size_t i = 2; i < node.children.size(); i++)
                    compileExpr(state, cs, node.children[i], dest);

                int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 1;
                cs.chunk.emitAsBx(OP_LOOP, 0, loopOffset, node.line);
                patchJump(cs, exitJump);
                cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
                for (int br : cs.loops.back().breaks) patchJump(cs, br);
                cs.loops.pop_back();
                fusedCmp = true;
                return dest;
            }
        }

        if (!fusedCmp) {
            uint8_t condReg = cs.allocReg();
            compileExpr(state, cs, node.children[1], condReg);
            int exitJump = emitJump(cs, OP_JMPFALSE, condReg, node.line);
            cs.freeReg();

            for (size_t i = 2; i < node.children.size(); i++)
                compileExpr(state, cs, node.children[i], dest);

            int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 1;
            cs.chunk.emitAsBx(OP_LOOP, 0, loopOffset, node.line);
            patchJump(cs, exitJump);
            cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
            for (int br : cs.loops.back().breaks) patchJump(cs, br);
            cs.loops.pop_back();
        }
        return dest;
    }

    if (op == "each") {
        if (node.children.size() < 4) throw Error{"'each' requires variable, container, and body at line " + std::to_string(node.line)};
        if (node.children[1].type != NodeType::Symbol)
            throw Error{"'each' variable must be a symbol at line " + std::to_string(node.line)};
        uint32_t varSym = state.resolveSymbol(node.children[1].stringVal);
        uint8_t savedNext = cs.nextReg;

        uint8_t containerReg = cs.allocReg();
        compileExpr(state, cs, node.children[2], containerReg);
        uint8_t lenReg = cs.allocReg();
        cs.chunk.emitABC(OP_LENGTH, lenReg, containerReg, 0, node.line);
        uint8_t idxReg = cs.allocReg();
        cs.chunk.emitAsBx(OP_LOADINT, idxReg, 0, node.line);
        uint8_t elemReg = cs.allocReg();
        cs.localRegs[varSym] = elemReg;

        CompilerState::LoopInfo loopInfo;
        loopInfo.start = static_cast<int>(cs.chunk.code.size());
        loopInfo.isEach = true;
        cs.loops.push_back(loopInfo);

        uint8_t cmpReg = cs.allocReg();
        cs.chunk.emitABC(OP_LT, cmpReg, idxReg, lenReg, node.line);
        int exitJump = emitJump(cs, OP_JMPFALSE, cmpReg, node.line);
        cs.freeReg();

        cs.chunk.emitABC(OP_GETINDEX, elemReg, containerReg, idxReg, node.line);
        for (size_t i = 3; i < node.children.size(); i++)
            compileExpr(state, cs, node.children[i], dest);

        for (int c : cs.loops.back().continues) patchJump(cs, c);
        size_t oneIdx = cs.chunk.addConstant(BblValue::makeInt(1));
        if (oneIdx > 255) throw Error{"too many constants (limit 256 for ADDK)"};
        cs.chunk.emitABC(OP_ADDK, idxReg, idxReg, static_cast<uint8_t>(oneIdx), node.line);

        int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 1;
        cs.chunk.emitAsBx(OP_LOOP, 0, loopOffset, node.line);

        patchJump(cs, exitJump);
        cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
        for (int br : cs.loops.back().breaks) patchJump(cs, br);
        cs.loops.pop_back();
        cs.freeRegsTo(savedNext);
        return dest;
    }

    if (op == "do") {
        if (node.children.size() <= 1) {
            cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
            return dest;
        }
        for (size_t i = 1; i < node.children.size(); i++)
            compileExpr(state, cs, node.children[i], dest);
        return dest;
    }

    if (op == "and") {
        if (node.children.size() < 3) throw Error{"'and' requires 2 arguments at line " + std::to_string(node.line)};
        compileExpr(state, cs, node.children[1], dest);
        int endJump = emitJump(cs, OP_AND, dest, node.line);
        compileExpr(state, cs, node.children[2], dest);
        patchJump(cs, endJump);
        return dest;
    }

    if (op == "or") {
        if (node.children.size() < 3) throw Error{"'or' requires 2 arguments at line " + std::to_string(node.line)};
        compileExpr(state, cs, node.children[1], dest);
        int endJump = emitJump(cs, OP_OR, dest, node.line);
        compileExpr(state, cs, node.children[2], dest);
        patchJump(cs, endJump);
        return dest;
    }

    if (op == "fn") {
        compileFn(state, cs, node, "", dest);
        return dest;
    }

    if (op == "break") {
        if (cs.loops.empty()) throw Error{"'break' outside of loop at line " + std::to_string(node.line)};
        int br = emitJump(cs, OP_JMP, 0, node.line);
        cs.loops.back().breaks.push_back(br);
        cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
        return dest;
    }

    if (op == "continue") {
        if (cs.loops.empty()) throw Error{"'continue' outside of loop at line " + std::to_string(node.line)};
        if (cs.loops.back().isEach) {
            int jmp = emitJump(cs, OP_JMP, 0, node.line);
            cs.loops.back().continues.push_back(jmp);
        } else {
            int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 1;
            cs.chunk.emitAsBx(OP_LOOP, 0, loopOffset, node.line);
        }
        return dest;
    }

    if (op == "with") {
        if (node.children.size() < 4) throw Error{"'with' requires name, initializer, and body at line " + std::to_string(node.line)};
        if (node.children[1].type != NodeType::Symbol) throw Error{"with: first argument must be a symbol at line " + std::to_string(node.line)};
        uint32_t varSym = state.resolveSymbol(node.children[1].stringVal);
        uint8_t varReg = cs.allocReg();
        cs.localRegs[varSym] = varReg;
        compileInto(state, cs, node.children[2], varReg);

        // Type check: must be userdata
        uint8_t helperReg = cs.allocReg();
        uint8_t helperArgReg = cs.allocReg();
        uint16_t checkSym = addConstIdx(cs,
            BblValue::makeInt(static_cast<int64_t>(state.resolveSymbol("__with_typecheck"))));
        cs.chunk.emitABx(OP_GETGLOBAL, helperReg, checkSym, node.line);
        cs.chunk.emitABC(OP_MOVE, helperArgReg, varReg, 0, node.line);
        cs.chunk.emitABC(OP_CALL, helperReg, 1, 0, node.line);

        int tryJump = emitJump(cs, OP_TRYBEGIN, dest, node.line);
        for (size_t i = 3; i < node.children.size(); i++)
            compileExpr(state, cs, node.children[i], dest);
        cs.chunk.emitABC(OP_TRYEND, 0, 0, 0, node.line);

        uint16_t cleanupSym = addConstIdx(cs,
            BblValue::makeInt(static_cast<int64_t>(state.resolveSymbol("__with_cleanup"))));
        cs.chunk.emitABx(OP_GETGLOBAL, helperReg, cleanupSym, node.line);
        cs.chunk.emitABC(OP_MOVE, helperArgReg, varReg, 0, node.line);
        cs.chunk.emitABC(OP_CALL, helperReg, 1, 0, node.line);
        int endJump = emitJump(cs, OP_JMP, 0, node.line);

        // Catch: cleanup then rethrow
        patchJump(cs, tryJump);
        uint8_t errReg = cs.allocReg();
        cs.chunk.emitABC(OP_MOVE, errReg, dest, 0, node.line);
        cs.chunk.emitABx(OP_GETGLOBAL, helperReg, cleanupSym, node.line);
        cs.chunk.emitABC(OP_MOVE, helperArgReg, varReg, 0, node.line);
        cs.chunk.emitABC(OP_CALL, helperReg, 1, 0, node.line);
        uint16_t rethrowSym = addConstIdx(cs,
            BblValue::makeInt(static_cast<int64_t>(state.resolveSymbol("__with_rethrow"))));
        cs.chunk.emitABx(OP_GETGLOBAL, helperReg, rethrowSym, node.line);
        cs.chunk.emitABC(OP_MOVE, helperArgReg, errReg, 0, node.line);
        cs.chunk.emitABC(OP_CALL, helperReg, 1, 0, node.line);
        patchJump(cs, endJump);
        return dest;
    }

    if (op == "try") {
        if (node.children.size() < 3) throw Error{"'try' requires body and catch at line " + std::to_string(node.line)};
        auto& catchNode = node.children.back();
        if (catchNode.type != NodeType::List || catchNode.children.empty() ||
            catchNode.children[0].type != NodeType::Symbol ||
            catchNode.children[0].stringVal != "catch" ||
            catchNode.children.size() < 3)
            throw Error{"try: last argument must be (catch var handler) at line " + std::to_string(node.line)};

        int tryJump = emitJump(cs, OP_TRYBEGIN, dest, node.line);
        for (size_t i = 1; i < node.children.size() - 1; i++)
            compileExpr(state, cs, node.children[i], dest);
        cs.chunk.emitABC(OP_TRYEND, 0, 0, 0, node.line);
        int endJump = emitJump(cs, OP_JMP, 0, node.line);
        patchJump(cs, tryJump);

        uint32_t catchSym = state.resolveSymbol(catchNode.children[1].stringVal);
        uint8_t catchReg = cs.allocReg();
        cs.localRegs[catchSym] = catchReg;
        if (catchReg != dest) cs.chunk.emitABC(OP_MOVE, catchReg, dest, 0, node.line);

        for (size_t i = 2; i < catchNode.children.size(); i++)
            compileExpr(state, cs, catchNode.children[i], dest);
        patchJump(cs, endJump);
        return dest;
    }

    if (op == "vector") {
        if (node.children.size() < 2) throw Error{"'vector' requires a type name at line " + std::to_string(node.line)};
        uint8_t savedNext = cs.nextReg;
        uint8_t base = dest;
        if (base < cs.nextReg) { base = cs.allocReg(); cs.freeRegsTo(base); }
        if (cs.nextReg <= base) cs.nextReg = base + 1;
        uint8_t typeIdx = addStrConst(state, cs, node.children[1].stringVal);
        uint8_t argc = checkArgc(node.children.size() - 2, node.line);
        for (size_t i = 2; i < node.children.size(); i++) {
            uint8_t argReg = cs.allocReg();
            compileInto(state, cs, node.children[i], argReg);
        }
        cs.chunk.emitABC(OP_VECTOR, base, argc, typeIdx, node.line);
        cs.freeRegsTo(savedNext);
        if (base != dest) cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
        return dest;
    }

    if (op == "table") {
        uint8_t savedNext = cs.nextReg;
        uint8_t base = dest;
        if (base < cs.nextReg) { base = cs.allocReg(); cs.freeRegsTo(base); }
        if (cs.nextReg <= base) cs.nextReg = base + 1;
        uint8_t pairCount = checkArgc((node.children.size() - 1) / 2, node.line);
        for (size_t i = 1; i < node.children.size(); i++) {
            uint8_t argReg = cs.allocReg();
            compileInto(state, cs, node.children[i], argReg);
        }
        cs.chunk.emitABC(OP_TABLE, base, pairCount, 0, node.line);
        cs.freeRegsTo(savedNext);
        if (base != dest) cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
        return dest;
    }

    if (op == "struct") {
        if (node.children.size() < 4 || node.children.size() % 2 != 0)
            throw Error{"struct: expected (struct Name type field ...) at line " + std::to_string(node.line)};
        if (node.children[1].type != NodeType::Symbol)
            throw Error{"struct: name must be a symbol at line " + std::to_string(node.line)};
        auto& sname = node.children[1].stringVal;
        if (state.structDescs().find(sname) != state.structDescs().end())
            throw Error{"struct " + sname + " already defined at line " + std::to_string(node.line)};

        static const std::unordered_map<std::string, std::pair<CType, size_t>> typeTable = {
            {"bool", {CType::Bool, 1}}, {"int8", {CType::Int8, 1}}, {"uint8", {CType::Uint8, 1}},
            {"int16", {CType::Int16, 2}}, {"uint16", {CType::Uint16, 2}},
            {"int32", {CType::Int32, 4}}, {"uint32", {CType::Uint32, 4}},
            {"int64", {CType::Int64, 8}}, {"uint64", {CType::Uint64, 8}},
            {"float32", {CType::Float32, 4}}, {"float64", {CType::Float64, 8}},
        };
        StructDesc desc;
        desc.name = sname;
        size_t offset = 0;
        for (size_t i = 2; i < node.children.size(); i += 2) {
            if (node.children[i].type != NodeType::Symbol || node.children[i+1].type != NodeType::Symbol)
                throw Error{"struct " + sname + ": expected type and field name symbols at line " + std::to_string(node.line)};
            auto& typeSym = node.children[i].stringVal;
            auto& fieldName = node.children[i+1].stringVal;
            for (auto& f : desc.fields)
                if (f.name == fieldName) throw Error{"struct " + sname + ": duplicate field name " + fieldName};
            auto tit = typeTable.find(typeSym);
            if (tit != typeTable.end()) {
                desc.fields.push_back(FieldDesc{fieldName, offset, tit->second.second, tit->second.first, ""});
                offset += tit->second.second;
            } else {
                auto sit = state.structDescs().find(typeSym);
                if (sit != state.structDescs().end()) {
                    desc.fields.push_back(FieldDesc{fieldName, offset, sit->second.totalSize, CType::Struct, typeSym});
                    offset += sit->second.totalSize;
                } else throw Error{"struct " + sname + ": unknown type " + typeSym};
            }
        }
        desc.totalSize = offset;
        state.structDescs()[sname] = desc;
        cs.chunk.emitABC(OP_LOADNULL, dest, 0, 0, node.line);
        return dest;
    }

    if (op == "binary") {
        if (node.children.size() != 2) throw Error{"'binary' requires exactly 1 argument at line " + std::to_string(node.line)};
        uint8_t srcReg = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_BINARY, dest, srcReg, 0, node.line);
        return dest;
    }

    if (op == "int") {
        if (node.children.size() != 2) throw Error{"'int' requires 1 argument at line " + std::to_string(node.line)};
        uint8_t srcReg = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_INT, dest, srcReg, 0, node.line);
        return dest;
    }

    if (op == "size-of") {
        if (node.children.size() < 2) throw Error{"'size-of' requires a type name at line " + std::to_string(node.line)};
        if (node.children[1].type != NodeType::Symbol) throw Error{"'size-of' argument must be a type name at line " + std::to_string(node.line)};
        uint8_t nameIdx = addStrConst(state, cs, node.children[1].stringVal);
        cs.chunk.emitABC(OP_SIZEOF, dest, nameIdx, 0, node.line);
        return dest;
    }

    if (op == "exec") {
        if (node.children.size() < 2) throw Error{"'exec' requires an argument at line " + std::to_string(node.line)};
        uint8_t srcReg = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_EXEC, dest, srcReg, 0, node.line);
        return dest;
    }

    if (op == "exec-file") {
        if (node.children.size() < 2) throw Error{"'exec-file' requires an argument at line " + std::to_string(node.line)};
        uint8_t srcReg = compileExpr(state, cs, node.children[1], dest);
        cs.chunk.emitABC(OP_EXECFILE, dest, srcReg, 0, node.line);
        return dest;
    }

    // Struct constructor
    auto it = state.structDescs().find(op);
    if (it != state.structDescs().end()) {
        uint8_t savedNext = cs.nextReg;
        uint8_t base = cs.allocReg(); // reserve base register
        uint8_t nameIdx = addStrConst(state, cs, op);
        uint8_t argc = checkArgc(node.children.size() - 1, node.line);
        for (size_t i = 1; i < node.children.size(); i++) {
            uint8_t argReg = cs.allocReg();
            compileInto(state, cs, node.children[i], argReg);
        }
        cs.chunk.emitABC(OP_STRUCT, base, argc, nameIdx, node.line);
        cs.freeRegsTo(savedNext);
        if (base != dest) cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
        return dest;
    }

    // Regular function call — try compile-time inlining first
    if (head.type == NodeType::Symbol) {
        uint32_t symId = state.resolveSymbol(head.stringVal);
        auto iit = cs.inlinableFns.find(symId);
        // Don't inline recursive calls or functions currently being inlined
        static thread_local std::unordered_set<uint32_t> inlining;
        if (iit != cs.inlinableFns.end() && inlining.find(symId) == inlining.end()) {
            inlining.insert(symId);
            const AstNode* fnNode = iit->second;
            auto& paramList = fnNode->children[1];
            auto& body = fnNode->children[2];
            size_t argc = node.children.size() - 1;

            if (argc == paramList.children.size()) {
                // Compile args into temp registers, map param symbols to those regs
                uint8_t savedNext = cs.nextReg;
                std::unordered_map<uint32_t, uint8_t> savedLocals;

                // Compile each argument and bind to param name
                for (size_t i = 0; i < argc; i++) {
                    uint32_t paramSym = state.resolveSymbol(paramList.children[i].stringVal);
                    uint8_t argReg = compileExpr(state, cs, node.children[i + 1], cs.allocReg());
                    // Save and override the local mapping
                    auto existing = cs.localRegs.find(paramSym);
                    if (existing != cs.localRegs.end())
                        savedLocals[paramSym] = existing->second;
                    cs.localRegs[paramSym] = argReg;
                }

                // Compile the inlined body
                uint8_t resultReg = compileExpr(state, cs, body, dest);
                if (resultReg != dest)
                    cs.chunk.emitABC(OP_MOVE, dest, resultReg, 0, node.line);

                // Restore param mappings
                for (size_t i = 0; i < argc; i++) {
                    uint32_t paramSym = state.resolveSymbol(paramList.children[i].stringVal);
                    auto sit = savedLocals.find(paramSym);
                    if (sit != savedLocals.end())
                        cs.localRegs[paramSym] = sit->second;
                    else
                        cs.localRegs.erase(paramSym);
                }
                cs.freeRegsTo(savedNext);
                inlining.erase(symId);
                return dest;
            }
            inlining.erase(symId);
        }
    }

    // Regular function call (not inlined)
    uint8_t base = dest;
    uint8_t savedNext = cs.nextReg;
    if (base < cs.nextReg) { base = cs.allocReg(); cs.freeRegsTo(base); }

    compileInto(state, cs, head, base);
    if (cs.nextReg <= base) cs.nextReg = base + 1;
    uint8_t argc = checkArgc(node.children.size() - 1, node.line);
    for (size_t i = 1; i < node.children.size(); i++) {
        uint8_t argReg = cs.allocReg();
        compileInto(state, cs, node.children[i], argReg);
    }
    bool isTailCall = node.isTailCall && !cs.fnName.empty();
    cs.chunk.emitABC(isTailCall ? OP_TAILCALL : OP_CALL, base, argc, 1, node.line);
    cs.freeRegsTo(savedNext);
    if (base != dest) cs.chunk.emitABC(OP_MOVE, dest, base, 0, node.line);
    return dest;
}

static void compileFn(BblState& state, CompilerState& cs, const AstNode& node, const std::string& assignName, uint8_t dest) {
    CompilerState fnCs;
    fnCs.enclosing = &cs;
    fnCs.scopeDepth = 1;
    fnCs.fnName = assignName;
    fnCs.compileDepth = cs.compileDepth;

    if (node.children.size() < 3) throw Error{"fn requires a parameter list and body at line " + std::to_string(node.line)};
    auto& paramList = node.children[1];
    if (paramList.type != NodeType::List) throw Error{"fn: first argument must be a parameter list at line " + std::to_string(node.line)};

    fnCs.allocReg(); // R[0] = callee
    for (auto& p : paramList.children) {
        if (p.type != NodeType::Symbol) throw Error{"fn: parameter must be a symbol at line " + std::to_string(node.line)};
        uint32_t symId = state.resolveSymbol(p.stringVal);
        uint8_t reg = fnCs.allocReg();
        fnCs.localRegs[symId] = reg;
    }
    fnCs.arity = static_cast<int>(paramList.children.size());

    uint8_t resultReg = fnCs.allocReg();
    uint8_t lastReg = resultReg;
    for (size_t i = 2; i < node.children.size(); i++)
        lastReg = compileExpr(state, fnCs, node.children[i], resultReg);
    if (node.children.size() <= 2) {
        fnCs.chunk.emitABC(OP_LOADNULL, resultReg, 0, 0, node.line);
        lastReg = resultReg;
    }
    if (lastReg != resultReg) {
        fnCs.chunk.emitABC(OP_MOVE, resultReg, lastReg, 0, node.line);
    }

    fnCs.chunk.emitABC(OP_RETURN, resultReg, 0, 0, node.line);
    fnCs.chunk.numRegs = fnCs.maxRegs;
    for (auto& [symId, reg] : fnCs.localRegs) {
        auto nit = state.symbolNames.find(symId);
        if (nit != state.symbolNames.end())
            fnCs.chunk.debugRegNames[reg] = nit->second->data;
    }

    BblClosure* proto = new BblClosure();
    proto->chunk = std::move(fnCs.chunk);
    proto->arity = fnCs.arity;
    proto->name = assignName;
    proto->env = state.currentEnv;
    proto->gcNext = state.nurseryHead; state.nurseryHead = proto;

    uint16_t protoIdx = addConstIdx(cs, BblValue::makeClosure(proto));
    cs.chunk.emitABx(OP_CLOSURE, dest, protoIdx, node.line);

    proto->captureDescs = std::move(fnCs.captures);
}

Chunk compile(BblState& state, const std::vector<AstNode>& nodes) {
    CompilerState cs;
    cs.scopeDepth = 0;
    cs.allocReg(); // R[0] reserved

    uint8_t resultReg = cs.allocReg();
    uint8_t lastReg = resultReg;
    for (size_t i = 0; i < nodes.size(); i++)
        lastReg = compileExpr(state, cs, nodes[i], resultReg);
    if (nodes.empty()) {
        cs.chunk.emitABC(OP_LOADNULL, resultReg, 0, 0, 0);
        lastReg = resultReg;
    }
    if (lastReg != resultReg)
        cs.chunk.emitABC(OP_MOVE, resultReg, lastReg, 0, nodes.empty() ? 0 : nodes.back().line);
    cs.chunk.emitABC(OP_RETURN, resultReg, 0, 0, nodes.empty() ? 0 : nodes.back().line);
    cs.chunk.numRegs = cs.maxRegs;
    for (auto& [symId, reg] : cs.localRegs) {
        auto nit = state.symbolNames.find(symId);
        if (nit != state.symbolNames.end())
            cs.chunk.debugRegNames[reg] = nit->second->data;
    }

    return std::move(cs.chunk);
}

} // namespace bbl
