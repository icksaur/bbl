#include "compiler.h"
#include "bbl.h"
#include "vm.h"
#include <stdexcept>

int CompilerState::resolveLocal(uint32_t symbolId) const {
    for (int i = static_cast<int>(locals.size()) - 1; i >= 0; i--) {
        if (locals[i].symbolId == symbolId) return i;
    }
    return -1;
}

int CompilerState::resolveCapture(uint32_t symbolId) {
    if (!enclosing) return -1;

    int local = enclosing->resolveLocal(symbolId);
    if (local != -1) {
        for (size_t i = 0; i < captures.size(); i++) {
            if (captures[i].srcType == 0 && captures[i].srcIdx == static_cast<uint16_t>(local))
                return static_cast<int>(i);
        }
        captures.push_back({0, static_cast<uint16_t>(local)});
        return static_cast<int>(captures.size() - 1);
    }

    int cap = enclosing->resolveCapture(symbolId);
    if (cap != -1) {
        for (size_t i = 0; i < captures.size(); i++) {
            if (captures[i].srcType == 1 && captures[i].srcIdx == static_cast<uint16_t>(cap))
                return static_cast<int>(i);
        }
        captures.push_back({1, static_cast<uint16_t>(cap)});
        return static_cast<int>(captures.size() - 1);
    }

    return -1;
}

static void emitConstant(CompilerState& cs, const BblValue& val, int line) {
    size_t idx = cs.chunk.addConstant(val);
    cs.chunk.emit(OP_CONSTANT, line);
    cs.chunk.emitU16(static_cast<uint16_t>(idx), line);
}

static int emitJump(CompilerState& cs, OpCode op, int line) {
    cs.chunk.emit(op, line);
    int offset = static_cast<int>(cs.chunk.code.size());
    cs.chunk.emitU16(0xffff, line);
    return offset;
}

static void patchJump(CompilerState& cs, int offset) {
    int jump = static_cast<int>(cs.chunk.code.size()) - offset - 2;
    cs.chunk.patchU16(offset, static_cast<uint16_t>(jump));
}

static bool compileNode(BblState& state, CompilerState& cs, const AstNode& node);
static bool compileList(BblState& state, CompilerState& cs, const AstNode& node);

static void emitGetVar(BblState& state, CompilerState& cs, uint32_t symId, int line) {
    int slot = cs.resolveLocal(symId);
    if (slot != -1) {
        cs.chunk.emit(OP_GET_LOCAL, line);
        cs.chunk.emitU16(static_cast<uint16_t>(slot), line);
        return;
    }
    int cap = cs.resolveCapture(symId);
    if (cap != -1) {
        cs.chunk.emit(OP_GET_CAPTURE, line);
        cs.chunk.emit(static_cast<uint8_t>(cap), line);
        return;
    }
    size_t idx = cs.chunk.addConstant(BblValue::makeInt(static_cast<int64_t>(symId)));
    cs.chunk.emit(OP_GET_GLOBAL, line);
    cs.chunk.emitU16(static_cast<uint16_t>(idx), line);
}

static bool emitSetVar(BblState& state, CompilerState& cs, uint32_t symId, int line, bool define) {
    int slot = cs.resolveLocal(symId);
    if (slot != -1) {
        cs.chunk.emit(OP_SET_LOCAL, line);
        cs.chunk.emitU16(static_cast<uint16_t>(slot), line);
        return false;
    }
    if (!define) {
        int cap = cs.resolveCapture(symId);
        if (cap != -1) {
            cs.chunk.emit(OP_SET_CAPTURE, line);
            cs.chunk.emit(static_cast<uint8_t>(cap), line);
            return false;
        }
    }
    if (define && cs.scopeDepth > 0) {
        cs.locals.push_back({symId, cs.scopeDepth});
        return true; // value stays on stack as local slot
    }
    size_t idx = cs.chunk.addConstant(BblValue::makeInt(static_cast<int64_t>(symId)));
    cs.chunk.emit(OP_SET_GLOBAL, line);
    cs.chunk.emitU16(static_cast<uint16_t>(idx), line);
    return false;
}

static bool compileNode(BblState& state, CompilerState& cs, const AstNode& node) {
    switch (node.type) {
        case NodeType::IntLiteral:
            emitConstant(cs, BblValue::makeInt(node.intVal), node.line);
            return false;
        case NodeType::FloatLiteral:
            emitConstant(cs, BblValue::makeFloat(node.floatVal), node.line);
            return false;
        case NodeType::StringLiteral:
            emitConstant(cs, BblValue::makeString(state.intern(node.stringVal)), node.line);
            return false;
        case NodeType::BoolLiteral:
            cs.chunk.emit(node.boolVal ? OP_TRUE : OP_FALSE, node.line);
            return false;
        case NodeType::NullLiteral:
            cs.chunk.emit(OP_NULL, node.line);
            return false;
        case NodeType::BinaryLiteral: {
            BblBinary* bin = state.allocBinary(node.binaryData);
            emitConstant(cs, BblValue::makeBinary(bin), node.line);
            return false;
        }
        case NodeType::Symbol: {
            uint32_t symId = state.resolveSymbol(node.stringVal);
            emitGetVar(state, cs, symId, node.line);
            return false;
        }
        case NodeType::List:
            return compileList(state, cs, node);
        case NodeType::DotAccess: {
            compileNode(state, cs, node.children[0]);
            if (!node.stringVal.empty()) {
                size_t idx = cs.chunk.addConstant(BblValue::makeString(state.intern(node.stringVal)));
                cs.chunk.emit(OP_GET_FIELD, node.line);
                cs.chunk.emitU16(static_cast<uint16_t>(idx), node.line);
            } else {
                emitConstant(cs, BblValue::makeInt(node.intVal), node.line);
                cs.chunk.emit(OP_GET_INDEX, node.line);
            }
            return false;
        }
        case NodeType::ColonAccess:
            throw BBL::Error{"colon access must be called as a method"};
    }
    return false;
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
    return OP_NULL; // sentinel
}

static OpCode cmpOp(const std::string& name) {
    if (name == "==") return OP_EQ;
    if (name == "!=") return OP_NEQ;
    if (name == "<") return OP_LT;
    if (name == ">") return OP_GT;
    if (name == "<=") return OP_LTE;
    if (name == ">=") return OP_GTE;
    return OP_NULL;
}

static void compileFn(BblState& state, CompilerState& cs, const AstNode& node, const std::string& assignName);

static bool compileList(BblState& state, CompilerState& cs, const AstNode& node) {
    if (node.children.empty()) {
        cs.chunk.emit(OP_NULL, node.line);
        return false;
    }

    auto& head = node.children[0];

    if (head.type == NodeType::ColonAccess) {
        compileNode(state, cs, head.children[0]);
        for (size_t i = 1; i < node.children.size(); i++)
            compileNode(state, cs, node.children[i]);
        size_t nameIdx = cs.chunk.addConstant(BblValue::makeString(state.intern(head.stringVal)));
        cs.chunk.emit(OP_METHOD_CALL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(nameIdx), node.line);
        cs.chunk.emit(static_cast<uint8_t>(node.children.size() - 1), node.line);
        return false;
    }

    if (head.type != NodeType::Symbol) {
        compileNode(state, cs, head);
        for (size_t i = 1; i < node.children.size(); i++)
            compileNode(state, cs, node.children[i]);
        cs.chunk.emit(OP_CALL, node.line);
        cs.chunk.emit(static_cast<uint8_t>(node.children.size() - 1), node.line);
        return false;
    }

    const std::string& op = head.stringVal;

    OpCode aop = arithOp(op);
    if (aop != OP_NULL) {
        if (node.children.size() < 3) throw BBL::Error{"'" + op + "' requires at least 2 arguments"};
        compileNode(state, cs, node.children[1]);
        for (size_t i = 2; i < node.children.size(); i++) {
            compileNode(state, cs, node.children[i]);
            cs.chunk.emit(aop, node.line);
        }
        return false;
    }

    OpCode cop = cmpOp(op);
    if (cop != OP_NULL) {
        if (node.children.size() != 3) throw BBL::Error{"'" + op + "' requires exactly 2 arguments"};
        compileNode(state, cs, node.children[1]);
        compileNode(state, cs, node.children[2]);
        cs.chunk.emit(cop, node.line);
        return false;
    }

    if (op == "shl" || op == "shr") {
        if (node.children.size() != 3) throw BBL::Error{"'" + op + "' requires exactly 2 arguments"};
        compileNode(state, cs, node.children[1]);
        compileNode(state, cs, node.children[2]);
        cs.chunk.emit(op == "shl" ? OP_SHL : OP_SHR, node.line);
        return false;
    }

    if (op == "bnot") {
        if (node.children.size() != 2) throw BBL::Error{"'bnot' requires exactly 1 argument"};
        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_BNOT, node.line);
        return false;
    }

    if (op == "not") {
        if (node.children.size() != 2) throw BBL::Error{"'not' requires exactly 1 argument"};
        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_NOT, node.line);
        return false;
    }

    if (op == "=") {
        if (node.children.size() < 3) throw BBL::Error{"'=' requires at least 2 arguments"};
        auto& target = node.children[1];

        if (target.type == NodeType::Symbol) {
            uint32_t symId = state.resolveSymbol(target.stringVal);
            bool isFnDef = node.children[2].type == NodeType::List &&
                           !node.children[2].children.empty() &&
                           node.children[2].children[0].type == NodeType::Symbol &&
                           node.children[2].children[0].stringVal == "fn";

            if (isFnDef) {
                compileFn(state, cs, node.children[2], target.stringVal);
            } else {
                compileNode(state, cs, node.children[2]);
            }

            bool exists = cs.resolveLocal(symId) != -1;
            if (!exists && cs.enclosing) {
                int cap = cs.resolveCapture(symId);
                if (cap != -1) exists = true;
            }
            bool isNewLocal = emitSetVar(state, cs, symId, node.line, !exists);
            if (isNewLocal) return true;
        } else if (target.type == NodeType::DotAccess) {
            if (!target.stringVal.empty()) {
                // SET_FIELD expects: [val, obj] (obj on top)
                compileNode(state, cs, node.children[2]); // value
                compileNode(state, cs, target.children[0]); // object
                size_t idx = cs.chunk.addConstant(BblValue::makeString(state.intern(target.stringVal)));
                cs.chunk.emit(OP_SET_FIELD, node.line);
                cs.chunk.emitU16(static_cast<uint16_t>(idx), node.line);
            } else {
                // SET_INDEX expects: [obj, idx, val] (val on top)
                compileNode(state, cs, target.children[0]); // object
                emitConstant(cs, BblValue::makeInt(target.intVal), node.line); // index
                compileNode(state, cs, node.children[2]); // value
                cs.chunk.emit(OP_SET_INDEX, node.line);
            }
        } else {
            throw BBL::Error{"invalid assignment target"};
        }
        return false;
    }

    if (op == "if") {
        compileNode(state, cs, node.children[1]);
        int elseJump = emitJump(cs, OP_JUMP_IF_FALSE, node.line);
        compileNode(state, cs, node.children[2]);
        if (node.children.size() > 3) {
            int endJump = emitJump(cs, OP_JUMP, node.line);
            patchJump(cs, elseJump);
            compileNode(state, cs, node.children[3]);
            patchJump(cs, endJump);
        } else {
            int endJump = emitJump(cs, OP_JUMP, node.line);
            patchJump(cs, elseJump);
            cs.chunk.emit(OP_NULL, node.line);
            patchJump(cs, endJump);
        }
        return false;
    }

    if (op == "loop") {
        CompilerState::LoopInfo loopInfo;
        loopInfo.start = static_cast<int>(cs.chunk.code.size());
        loopInfo.scopeDepthAtLoop = cs.scopeDepth;
        cs.loops.push_back(loopInfo);

        compileNode(state, cs, node.children[1]);
        int exitJump = emitJump(cs, OP_JUMP_IF_FALSE, node.line);

        for (size_t i = 2; i < node.children.size(); i++) {
            compileNode(state, cs, node.children[i]);
            cs.chunk.emit(OP_POP, node.line);
        }

        int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 3;
        cs.chunk.emit(OP_LOOP, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(loopOffset), node.line);

        patchJump(cs, exitJump);
        cs.chunk.emit(OP_NULL, node.line);

        for (int br : cs.loops.back().breaks) patchJump(cs, br);
        cs.loops.pop_back();
        return false;
    }

    if (op == "each") {
        if (node.children.size() < 4)
            throw BBL::Error{"'each' requires variable, container, and body"};

        auto& varNode = node.children[1];
        uint32_t varSym = state.resolveSymbol(varNode.stringVal);

        // Reserve slots for each's internal locals by pushing nulls
        int containerSlot = static_cast<int>(cs.locals.size());
        cs.locals.push_back({state.resolveSymbol("__each_container__"), cs.scopeDepth});
        int lenSlot = static_cast<int>(cs.locals.size());
        cs.locals.push_back({state.resolveSymbol("__each_len__"), cs.scopeDepth});
        int idxSlot = static_cast<int>(cs.locals.size());
        cs.locals.push_back({state.resolveSymbol("__each_i__"), cs.scopeDepth});
        int elemSlot = static_cast<int>(cs.locals.size());
        cs.locals.push_back({varSym, cs.scopeDepth});

        // Push placeholder nulls to reserve stack space for these locals
        cs.chunk.emit(OP_NULL, node.line);  // container
        cs.chunk.emit(OP_NULL, node.line);  // len
        cs.chunk.emit(OP_NULL, node.line);  // i
        cs.chunk.emit(OP_NULL, node.line);  // elem

        // Store container
        compileNode(state, cs, node.children[2]);
        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(containerSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        // Store length
        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(containerSlot), node.line);
        cs.chunk.emit(OP_LENGTH, node.line);
        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(lenSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        // Init index to 0
        emitConstant(cs, BblValue::makeInt(0), node.line);
        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idxSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        CompilerState::LoopInfo loopInfo;
        loopInfo.start = static_cast<int>(cs.chunk.code.size());
        loopInfo.scopeDepthAtLoop = cs.scopeDepth;
        cs.loops.push_back(loopInfo);

        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idxSlot), node.line);
        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(lenSlot), node.line);
        cs.chunk.emit(OP_LT, node.line);
        int exitJump = emitJump(cs, OP_JUMP_IF_FALSE, node.line);

        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(containerSlot), node.line);
        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idxSlot), node.line);
        cs.chunk.emit(OP_GET_INDEX, node.line);
        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(elemSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        for (size_t i = 3; i < node.children.size(); i++) {
            compileNode(state, cs, node.children[i]);
            cs.chunk.emit(OP_POP, node.line);
        }

        cs.chunk.emit(OP_GET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idxSlot), node.line);
        emitConstant(cs, BblValue::makeInt(1), node.line);
        cs.chunk.emit(OP_ADD, node.line);
        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idxSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 3;
        cs.chunk.emit(OP_LOOP, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(loopOffset), node.line);

        patchJump(cs, exitJump);
        cs.chunk.emit(OP_NULL, node.line);

        for (int br : cs.loops.back().breaks) patchJump(cs, br);
        cs.loops.pop_back();
        return false;
    }

    if (op == "do") {
        if (node.children.size() <= 1) {
            cs.chunk.emit(OP_NULL, node.line);
            return false;
        }
        bool prevLocal = false;
        for (size_t i = 1; i < node.children.size(); i++) {
            if (i > 1 && !prevLocal) cs.chunk.emit(OP_POP, node.line);
            prevLocal = compileNode(state, cs, node.children[i]);
        }
        return false;
    }

    if (op == "and") {
        compileNode(state, cs, node.children[1]);
        int endJump = emitJump(cs, OP_AND, node.line);
        compileNode(state, cs, node.children[2]);
        patchJump(cs, endJump);
        return false;
    }

    if (op == "or") {
        compileNode(state, cs, node.children[1]);
        int endJump = emitJump(cs, OP_OR, node.line);
        compileNode(state, cs, node.children[2]);
        patchJump(cs, endJump);
        return false;
    }

    if (op == "fn") {
        compileFn(state, cs, node, "");
        return false;
    }

    if (op == "break") {
        if (cs.loops.empty()) throw BBL::Error{"'break' outside of loop"};
        int br = emitJump(cs, OP_JUMP, node.line);
        cs.loops.back().breaks.push_back(br);
        cs.chunk.emit(OP_NULL, node.line);
        return false;
    }

    if (op == "continue") {
        if (cs.loops.empty()) throw BBL::Error{"'continue' outside of loop"};
        int loopOffset = static_cast<int>(cs.chunk.code.size()) - cs.loops.back().start + 3;
        cs.chunk.emit(OP_LOOP, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(loopOffset), node.line);
        return false;
    }

    if (op == "try") {
        if (node.children.size() < 4) throw BBL::Error{"'try' requires body and catch"};
        int tryJump = emitJump(cs, OP_TRY_BEGIN, node.line);

        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_TRY_END, node.line);
        int endJump = emitJump(cs, OP_JUMP, node.line);

        patchJump(cs, tryJump);

        auto& catchVar = node.children[2];
        uint32_t catchSym = state.resolveSymbol(catchVar.stringVal);
        int catchSlot = static_cast<int>(cs.locals.size());
        cs.locals.push_back({catchSym, cs.scopeDepth});

        cs.chunk.emit(OP_SET_LOCAL, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(catchSlot), node.line);
        cs.chunk.emit(OP_POP, node.line);

        compileNode(state, cs, node.children[3]);
        patchJump(cs, endJump);
        return false;
    }

    if (op == "with") {
        throw BBL::Error{"'with' not yet implemented in bytecode"};
    }

    if (op == "vector") {
        if (node.children.size() < 2) throw BBL::Error{"'vector' requires a type name"};
        emitConstant(cs, BblValue::makeString(state.intern(node.children[1].stringVal)), node.line);
        for (size_t i = 2; i < node.children.size(); i++)
            compileNode(state, cs, node.children[i]);
        cs.chunk.emit(OP_VECTOR, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(node.children.size() - 2), node.line);
        return false;
    }

    if (op == "table") {
        size_t pairCount = (node.children.size() - 1) / 2;
        for (size_t i = 1; i < node.children.size(); i++)
            compileNode(state, cs, node.children[i]);
        cs.chunk.emit(OP_TABLE, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(pairCount), node.line);
        return false;
    }

    if (op == "struct") {
        // struct declaration must be handled at runtime (StructBuilder API requires C++ type info)
        // Fall through to tree-walker for declaration
        throw BBL::Error{"'struct' declarations in bytecode mode must use tree-walker (define structs before enabling --bytecode)"};
    }

    if (op == "binary") {
        if (node.children.size() != 2) throw BBL::Error{"'binary' requires exactly 1 argument"};
        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_BINARY, node.line);
        return false;
    }

    if (op == "sizeof") {
        if (node.children.size() != 2) throw BBL::Error{"'sizeof' requires exactly 1 argument"};
        size_t idx = cs.chunk.addConstant(BblValue::makeString(state.intern(node.children[1].stringVal)));
        cs.chunk.emit(OP_SIZEOF, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idx), node.line);
        return false;
    }

    if (op == "exec") {
        if (node.children.size() != 2) throw BBL::Error{"'exec' requires exactly 1 argument"};
        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_EXEC, node.line);
        return false;
    }

    if (op == "execfile") {
        if (node.children.size() != 2) throw BBL::Error{"'execfile' requires exactly 1 argument"};
        compileNode(state, cs, node.children[1]);
        cs.chunk.emit(OP_EXECFILE, node.line);
        return false;
    }

    // Check if it's a struct constructor
    auto it = state.structDescs.find(op);
    if (it != state.structDescs.end()) {
        for (size_t i = 1; i < node.children.size(); i++)
            compileNode(state, cs, node.children[i]);
        size_t idx = cs.chunk.addConstant(BblValue::makeString(state.intern(op)));
        cs.chunk.emit(OP_STRUCT, node.line);
        cs.chunk.emitU16(static_cast<uint16_t>(idx), node.line);
        cs.chunk.emit(static_cast<uint8_t>(node.children.size() - 1), node.line);
        return false;
    }

    // Regular function call
    compileNode(state, cs, head);
    for (size_t i = 1; i < node.children.size(); i++)
        compileNode(state, cs, node.children[i]);

    bool isTailCall = node.isTailCall && !cs.fnName.empty();
    cs.chunk.emit(isTailCall ? OP_TAIL_CALL : OP_CALL, node.line);
    cs.chunk.emit(static_cast<uint8_t>(node.children.size() - 1), node.line);
    return false;
}

static void compileFn(BblState& state, CompilerState& cs, const AstNode& node, const std::string& assignName) {
    CompilerState fnCs;
    fnCs.enclosing = &cs;
    fnCs.scopeDepth = 1;
    fnCs.fnName = assignName;

    if (node.children.size() < 3)
        throw BBL::Error{"fn requires a parameter list and body"};

    auto& paramList = node.children[1];
    if (paramList.type != NodeType::List)
        throw BBL::Error{"fn: first argument must be a parameter list"};

    // Slot 0 is reserved for the callee value on the stack
    fnCs.locals.push_back({0, 1});

    for (auto& p : paramList.children) {
        if (p.type != NodeType::Symbol)
            throw BBL::Error{"fn: parameter must be a symbol"};
        uint32_t symId = state.resolveSymbol(p.stringVal);
        fnCs.locals.push_back({symId, 1});
    }
    fnCs.arity = static_cast<int>(paramList.children.size());

    bool prevWasLocalDef = false;
    for (size_t i = 2; i < node.children.size(); i++) {
        if (i > 2 && !prevWasLocalDef) fnCs.chunk.emit(OP_POP, node.children[i].line);
        prevWasLocalDef = compileNode(state, fnCs, node.children[i]);
    }
    if (node.children.size() <= 2) {
        fnCs.chunk.emit(OP_NULL, node.line);
    } else if (prevWasLocalDef) {
        // Last expression was a local def — push its value as the return value
        int lastSlot = static_cast<int>(fnCs.locals.size()) - 1;
        fnCs.chunk.emit(OP_GET_LOCAL, node.line);
        fnCs.chunk.emitU16(static_cast<uint16_t>(lastSlot), node.line);
    }
    fnCs.chunk.emit(OP_RETURN, node.line);

    if (!assignName.empty()) {
        // Tail call marking is done at AST level via node.isTailCall
        // The compiler checks node.isTailCall when emitting CALL instructions
    }

    BblClosure* proto = new BblClosure();
    proto->chunk = std::move(fnCs.chunk);
    proto->arity = fnCs.arity;
    proto->name = assignName;
    state.allocatedClosures.push_back(proto);

    size_t protoIdx = cs.chunk.addConstant(BblValue::makeClosure(proto));
    cs.chunk.emit(OP_CLOSURE, node.line);
    cs.chunk.emitU16(static_cast<uint16_t>(protoIdx), node.line);
    cs.chunk.emit(static_cast<uint8_t>(fnCs.captures.size()), node.line);

    for (auto& cap : fnCs.captures) {
        cs.chunk.emit(cap.srcType, node.line);
        cs.chunk.emitU16(cap.srcIdx, node.line);
    }
}

Chunk compile(BblState& state, const std::vector<AstNode>& nodes) {
    CompilerState cs;
    cs.scopeDepth = 0;

    for (size_t i = 0; i < nodes.size(); i++) {
        if (i > 0) cs.chunk.emit(OP_POP, nodes[i].line);
        compileNode(state, cs, nodes[i]);
    }
    if (nodes.empty()) cs.chunk.emit(OP_NULL, 0);
    cs.chunk.emit(OP_RETURN, nodes.empty() ? 0 : nodes.back().line);

    return std::move(cs.chunk);
}
