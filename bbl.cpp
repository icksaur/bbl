#include "bbl.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <cinttypes>
#include <cstdio>
#include <filesystem>

// ---------- BblValue ----------

bool BblValue::operator==(const BblValue& o) const {
    if (type != o.type) {
        return false;
    }
    switch (type) {
        case BBL::Type::Null:   return true;
        case BBL::Type::Bool:   return boolVal == o.boolVal;
        case BBL::Type::Int:    return intVal == o.intVal;
        case BBL::Type::Float:  return floatVal == o.floatVal;
        case BBL::Type::String: return stringVal == o.stringVal;
        case BBL::Type::Binary: return binaryVal == o.binaryVal;
        case BBL::Type::Fn:     return fnVal == o.fnVal;
        default:                return false;
    }
}

// ---------- BblScope ----------

void BblScope::def(const std::string& name, BblValue val) {
    bindings[name] = val;
}

void BblScope::set(const std::string& name, BblValue val) {
    for (BblScope* s = this; s; s = s->parent) {
        auto it = s->bindings.find(name);
        if (it != s->bindings.end()) {
            it->second = val;
            return;
        }
    }
    throw BBL::Error{"undefined symbol: " + name};
}

BblValue* BblScope::lookup(const std::string& name) {
    for (BblScope* s = this; s; s = s->parent) {
        auto it = s->bindings.find(name);
        if (it != s->bindings.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

// ---------- Lexer ----------

BblLexer::BblLexer(const char* source) : src(source), len(static_cast<int>(strlen(source))) {}

char BblLexer::peek() const {
    if (pos >= len) {
        return '\0';
    }
    return src[pos];
}

char BblLexer::advance() {
    char c = src[pos++];
    if (c == '\n') {
        line++;
    }
    return c;
}

void BblLexer::skipWhitespaceAndComments() {
    while (pos < len) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            advance();
        } else if (c == '/' && pos + 1 < len && src[pos + 1] == '/') {
            while (pos < len && peek() != '\n') {
                advance();
            }
        } else {
            break;
        }
    }
}

Token BblLexer::readString() {
    int startLine = line;
    advance(); // skip opening "
    std::string result;
    while (pos < len && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (pos >= len) {
                throw BBL::Error{"unterminated string at line " + std::to_string(startLine)};
            }
            char esc = advance();
            switch (esc) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                default:   result += esc; break;
            }
        } else {
            result += advance();
        }
    }
    if (pos >= len) {
        throw BBL::Error{"unterminated string at line " + std::to_string(startLine)};
    }
    advance(); // skip closing "
    Token t;
    t.type = TokenType::String;
    t.stringVal = std::move(result);
    t.line = startLine;
    return t;
}

Token BblLexer::readNumber() {
    int startLine = line;
    int start = pos;
    bool isNegative = (peek() == '-');
    if (isNegative) {
        pos++;
    }

    // Check for binary literal: 0b
    if (!isNegative && peek() == '0' && pos + 1 < len && src[pos + 1] == 'b') {
        return readBinary();
    }

    bool isFloat = false;
    while (pos < len && (peek() >= '0' && peek() <= '9')) {
        pos++;
    }
    if (pos < len && peek() == '.') {
        isFloat = true;
        pos++;
        while (pos < len && (peek() >= '0' && peek() <= '9')) {
            pos++;
        }
    }

    std::string numStr(src + start, src + pos);
    Token t;
    t.line = startLine;
    if (isFloat) {
        t.type = TokenType::Float;
        t.floatVal = std::stod(numStr);
    } else {
        t.type = TokenType::Int;
        t.intVal = std::stoll(numStr);
    }
    return t;
}

Token BblLexer::readBinary() {
    int startLine = line;
    pos += 2; // skip 0b
    int sizeStart = pos;
    while (pos < len && peek() >= '0' && peek() <= '9') {
        pos++;
    }
    if (pos >= len || peek() != ':') {
        throw BBL::Error{"invalid binary literal at line " + std::to_string(startLine)};
    }
    size_t size = std::stoull(std::string(src + sizeStart, src + pos));
    pos++; // skip :
    if (static_cast<size_t>(len - pos) < size) {
        throw BBL::Error{"binary literal: insufficient bytes at line " + std::to_string(startLine)};
    }
    Token t;
    t.type = TokenType::Binary;
    t.binaryData.assign(reinterpret_cast<const uint8_t*>(src + pos),
                        reinterpret_cast<const uint8_t*>(src + pos + size));
    t.line = startLine;
    // Advance pos through raw bytes, tracking newlines
    for (size_t i = 0; i < size; i++) {
        if (src[pos] == '\n') {
            line++;
        }
        pos++;
    }
    return t;
}

static bool isSymbolStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isSymbolChar(char c) {
    return isSymbolStart(c) || (c >= '0' && c <= '9') || c == '-';
}

static bool isOperatorChar(char c) {
    return c == '+' || c == '*' || c == '/' || c == '%'
        || c == '=' || c == '!' || c == '<' || c == '>';
}

Token BblLexer::readSymbolOrKeyword() {
    int startLine = line;
    int start = pos;

    if (isOperatorChar(peek())) {
        // Read operator symbol
        while (pos < len && isOperatorChar(peek())) {
            pos++;
        }
    } else {
        // Read regular symbol
        while (pos < len && isSymbolChar(peek())) {
            pos++;
        }
    }

    std::string sym(src + start, src + pos);
    Token t;
    t.line = startLine;

    if (sym == "true") {
        t.type = TokenType::Bool;
        t.boolVal = true;
    } else if (sym == "false") {
        t.type = TokenType::Bool;
        t.boolVal = false;
    } else if (sym == "null") {
        t.type = TokenType::Null;
    } else {
        t.type = TokenType::Symbol;
        t.stringVal = std::move(sym);
    }
    return t;
}

Token BblLexer::nextToken() {
    skipWhitespaceAndComments();
    if (pos >= len) {
        Token t;
        t.type = TokenType::Eof;
        t.line = line;
        return t;
    }

    char c = peek();

    if (c == '(') {
        advance();
        return {TokenType::LParen, 0, 0, false, "", {}, line};
    }
    if (c == ')') {
        advance();
        return {TokenType::RParen, 0, 0, false, "", {}, line};
    }
    if (c == '"') {
        return readString();
    }

    // Negative number: - followed by digit (but only when not inside a symbol context)
    if (c == '-' && pos + 1 < len && src[pos + 1] >= '0' && src[pos + 1] <= '9') {
        return readNumber();
    }

    if ((c >= '0' && c <= '9')) {
        return readNumber();
    }

    // The - symbol by itself or operator symbols
    if (c == '-' || isOperatorChar(c)) {
        return readSymbolOrKeyword();
    }

    if (c == '.') {
        advance();
        return {TokenType::Dot, 0, 0, false, "", {}, line};
    }

    if (isSymbolStart(c)) {
        return readSymbolOrKeyword();
    }

    throw BBL::Error{"unexpected character '" + std::string(1, c) + "' at line " + std::to_string(line)};
}

// ---------- Parser ----------

static AstNode parseExpr(BblLexer& lexer, Token& tok);

static AstNode parsePrimary(BblLexer& lexer, Token& tok) {
    AstNode node;
    node.line = tok.line;

    switch (tok.type) {
        case TokenType::Int:
            node.type = NodeType::IntLiteral;
            node.intVal = tok.intVal;
            break;
        case TokenType::Float:
            node.type = NodeType::FloatLiteral;
            node.floatVal = tok.floatVal;
            break;
        case TokenType::String:
            node.type = NodeType::StringLiteral;
            node.stringVal = std::move(tok.stringVal);
            break;
        case TokenType::Bool:
            node.type = NodeType::BoolLiteral;
            node.boolVal = tok.boolVal;
            break;
        case TokenType::Null:
            node.type = NodeType::NullLiteral;
            break;
        case TokenType::Binary:
            node.type = NodeType::BinaryLiteral;
            node.binaryData = std::move(tok.binaryData);
            break;
        case TokenType::Symbol:
            node.type = NodeType::Symbol;
            node.stringVal = std::move(tok.stringVal);
            break;
        case TokenType::LParen: {
            node.type = NodeType::List;
            tok = lexer.nextToken();
            while (tok.type != TokenType::RParen) {
                if (tok.type == TokenType::Eof) {
                    throw BBL::Error{"parse error: expected ')' at line " + std::to_string(lexer.currentLine())};
                }
                node.children.push_back(parseExpr(lexer, tok));
            }
            break;
        }
        default:
            throw BBL::Error{"parse error: unexpected token at line " + std::to_string(tok.line)};
    }
    return node;
}

static AstNode parseExpr(BblLexer& lexer, Token& tok) {
    AstNode lhs = parsePrimary(lexer, tok);

    // Handle dot access: v.x or v.x.y (chained)
    while (true) {
        Token next = lexer.nextToken();
        if (next.type == TokenType::Dot) {
            Token field = lexer.nextToken();
            if (field.type != TokenType::Symbol) {
                throw BBL::Error{"parse error: expected field name after '.' at line " + std::to_string(field.line)};
            }
            AstNode dot;
            dot.type = NodeType::DotAccess;
            dot.stringVal = std::move(field.stringVal);
            dot.children.push_back(std::move(lhs));
            dot.line = next.line;
            lhs = std::move(dot);
        } else {
            // Put the token back by storing it for the caller
            // We need a way to "unread" — restructure: we consume the dot chain
            // and then the caller gets the next token from us.
            // Since the parser protocol is: caller passes in current token,
            // we modify tok to be the "lookahead" we didn't consume.
            tok = next;
            return lhs;
        }
    }
}

std::vector<AstNode> parse(BblLexer& lexer) {
    std::vector<AstNode> nodes;
    Token tok = lexer.nextToken();
    while (tok.type != TokenType::Eof) {
        if (tok.type == TokenType::RParen) {
            throw BBL::Error{"parse error: unexpected ')' at line " + std::to_string(tok.line)};
        }
        nodes.push_back(parseExpr(lexer, tok));
        // After parseExpr, tok holds the next unprocessed token
        // (it was set by the dot-access lookahead)
    }
    return nodes;
}

// ---------- BblState ----------

BblState::BblState() = default;

BblState::~BblState() {
    for (auto* s : allocatedStrings) {
        delete s;
    }
    for (auto* b : allocatedBinaries) {
        delete b;
    }
    for (auto* f : allocatedFns) {
        delete f;
    }
}

BblString* BblState::intern(const std::string& s) {
    auto it = internTable.find(s);
    if (it != internTable.end()) {
        return it->second;
    }
    auto* str = new BblString{s};
    allocatedStrings.push_back(str);
    internTable[str->data] = str;
    return str;
}

BblBinary* BblState::allocBinary(std::vector<uint8_t> data) {
    auto* b = new BblBinary{std::move(data)};
    allocatedBinaries.push_back(b);
    return b;
}

BblFn* BblState::allocFn() {
    auto* f = new BblFn{};
    allocatedFns.push_back(f);
    return f;
}

// ---------- Eval ----------

static std::string typeName(BBL::Type t) {
    switch (t) {
        case BBL::Type::Null:     return "null";
        case BBL::Type::Bool:     return "bool";
        case BBL::Type::Int:      return "int";
        case BBL::Type::Float:    return "float";
        case BBL::Type::String:   return "string";
        case BBL::Type::Binary:   return "binary";
        case BBL::Type::Fn:       return "fn";
        case BBL::Type::Vector:   return "vector";
        case BBL::Type::Table:    return "table";
        case BBL::Type::Struct:   return "struct";
        case BBL::Type::UserData: return "userdata";
    }
    return "unknown";
}

BblValue BblState::eval(const AstNode& node, BblScope& scope) {
    switch (node.type) {
        case NodeType::IntLiteral:
            return BblValue::makeInt(node.intVal);
        case NodeType::FloatLiteral:
            return BblValue::makeFloat(node.floatVal);
        case NodeType::StringLiteral:
            return BblValue::makeString(intern(node.stringVal));
        case NodeType::BoolLiteral:
            return BblValue::makeBool(node.boolVal);
        case NodeType::NullLiteral:
            return BblValue::makeNull();
        case NodeType::BinaryLiteral:
            return BblValue::makeBinary(allocBinary(node.binaryData));
        case NodeType::Symbol: {
            BblValue* v = scope.lookup(node.stringVal);
            if (!v) {
                throw BBL::Error{"undefined symbol: " + node.stringVal};
            }
            return *v;
        }
        case NodeType::List:
            return evalList(node, scope);
        case NodeType::DotAccess:
            throw BBL::Error{"dot access not yet implemented"};
    }
    throw BBL::Error{"internal: unknown node type"};
}

// Collect free variables from an AST body that are not in the bound set

static void gatherFreeVars(const AstNode& node,
                           const std::vector<std::string>& bound,
                           std::vector<std::string>& freeVars);

static void gatherFreeVarsBody(const std::vector<AstNode>& body,
                               std::vector<std::string> bound,
                               std::vector<std::string>& freeVars) {
    for (auto& n : body) {
        // If it's a def form, add the name to bound AFTER processing value
        if (n.type == NodeType::List && !n.children.empty()
            && n.children[0].type == NodeType::Symbol
            && n.children[0].stringVal == "def" && n.children.size() >= 3) {
            gatherFreeVars(n.children[2], bound, freeVars);
            bound.push_back(n.children[1].stringVal);
        } else {
            gatherFreeVars(n, bound, freeVars);
        }
    }
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (auto& e : v) {
        if (e == s) {
            return true;
        }
    }
    return false;
}

static void gatherFreeVars(const AstNode& node,
                           const std::vector<std::string>& bound,
                           std::vector<std::string>& freeVars) {
    switch (node.type) {
        case NodeType::Symbol:
            if (!contains(bound, node.stringVal) && !contains(freeVars, node.stringVal)) {
                freeVars.push_back(node.stringVal);
            }
            break;
        case NodeType::List:
            if (!node.children.empty() && node.children[0].type == NodeType::Symbol) {
                auto& op = node.children[0].stringVal;
                if (op == "fn") {
                    // fn creates its own scope — params are bound inside
                    if (node.children.size() >= 2 && node.children[1].type == NodeType::List) {
                        std::vector<std::string> inner = bound;
                        for (auto& p : node.children[1].children) {
                            if (p.type == NodeType::Symbol) {
                                inner.push_back(p.stringVal);
                            }
                        }
                        std::vector<AstNode> fnBody(node.children.begin() + 2, node.children.end());
                        gatherFreeVarsBody(fnBody, inner, freeVars);
                    }
                    return;
                }
                if (op == "def") {
                    // handled by gatherFreeVarsBody
                    for (size_t i = 1; i < node.children.size(); i++) {
                        gatherFreeVars(node.children[i], bound, freeVars);
                    }
                    return;
                }
            }
            for (auto& c : node.children) {
                gatherFreeVars(c, bound, freeVars);
            }
            break;
        case NodeType::DotAccess:
            for (auto& c : node.children) {
                gatherFreeVars(c, bound, freeVars);
            }
            break;
        default:
            break;
    }
}

BblValue BblState::evalList(const AstNode& node, BblScope& scope) {
    if (node.children.empty()) {
        return BblValue::makeNull();
    }

    auto& head = node.children[0];

    // Special forms
    if (head.type == NodeType::Symbol) {
        auto& op = head.stringVal;

        if (op == "def") {
            if (node.children.size() < 3) {
                throw BBL::Error{"def requires a name and value"};
            }
            auto& name = node.children[1];
            if (name.type != NodeType::Symbol) {
                throw BBL::Error{"def: first argument must be a symbol"};
            }
            BblValue val = eval(node.children[2], scope);
            scope.def(name.stringVal, val);
            return BblValue::makeNull();
        }

        if (op == "set") {
            if (node.children.size() < 3) {
                throw BBL::Error{"set requires a name and value"};
            }
            auto& target = node.children[1];
            if (target.type != NodeType::Symbol) {
                throw BBL::Error{"set: first argument must be a symbol"};
            }
            BblValue val = eval(node.children[2], scope);
            scope.set(target.stringVal, val);
            return BblValue::makeNull();
        }

        if (op == "if") {
            if (node.children.size() < 3) {
                throw BBL::Error{"if requires a condition and then-body"};
            }
            BblValue cond = eval(node.children[1], scope);
            if (cond.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: condition must be bool, got " + typeName(cond.type)};
            }
            if (cond.boolVal) {
                eval(node.children[2], scope);
            } else if (node.children.size() >= 4) {
                eval(node.children[3], scope);
            }
            return BblValue::makeNull();
        }

        if (op == "loop") {
            if (node.children.size() < 3) {
                throw BBL::Error{"loop requires a condition and body"};
            }
            while (true) {
                BblValue cond = eval(node.children[1], scope);
                if (cond.type != BBL::Type::Bool) {
                    throw BBL::Error{"type mismatch: condition must be bool, got " + typeName(cond.type)};
                }
                if (!cond.boolVal) {
                    break;
                }
                for (size_t i = 2; i < node.children.size(); i++) {
                    eval(node.children[i], scope);
                }
            }
            return BblValue::makeNull();
        }

        if (op == "and") {
            if (node.children.size() != 3) {
                throw BBL::Error{"and requires exactly 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            if (left.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: and requires bool, got " + typeName(left.type)};
            }
            if (!left.boolVal) {
                return BblValue::makeBool(false);
            }
            BblValue right = eval(node.children[2], scope);
            if (right.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: and requires bool, got " + typeName(right.type)};
            }
            return BblValue::makeBool(right.boolVal);
        }

        if (op == "or") {
            if (node.children.size() != 3) {
                throw BBL::Error{"or requires exactly 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            if (left.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: or requires bool, got " + typeName(left.type)};
            }
            if (left.boolVal) {
                return BblValue::makeBool(true);
            }
            BblValue right = eval(node.children[2], scope);
            if (right.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: or requires bool, got " + typeName(right.type)};
            }
            return BblValue::makeBool(right.boolVal);
        }

        if (op == "fn") {
            if (node.children.size() < 3) {
                throw BBL::Error{"fn requires a parameter list and body"};
            }
            auto& paramList = node.children[1];
            if (paramList.type != NodeType::List) {
                throw BBL::Error{"fn: first argument must be a parameter list"};
            }
            BblFn* fn = allocFn();
            for (auto& p : paramList.children) {
                if (p.type != NodeType::Symbol) {
                    throw BBL::Error{"fn: parameter must be a symbol"};
                }
                fn->params.push_back(p.stringVal);
            }
            fn->body.assign(node.children.begin() + 2, node.children.end());

            // Capture free variables
            std::vector<std::string> freeVars;
            std::vector<std::string> bound = fn->params;
            gatherFreeVarsBody(fn->body, bound, freeVars);

            // Filter: only capture names that are special forms or builtins
            // Actually, capture everything that exists in scope
            static const std::vector<std::string> specialForms = {
                "def", "set", "if", "loop", "and", "or", "fn", "exec", "not"
            };
            for (auto& name : freeVars) {
                if (contains(specialForms, name)) {
                    continue;
                }
                BblValue* val = scope.lookup(name);
                if (val) {
                    fn->captures.emplace_back(name, *val);
                }
                // If not found in scope, it might be defined later or be an error at call time
            }

            return BblValue::makeFn(fn);
        }

        if (op == "exec") {
            if (node.children.size() < 2) {
                throw BBL::Error{"exec requires a code string argument"};
            }
            BblValue codeVal = eval(node.children[1], scope);
            if (codeVal.type != BBL::Type::String) {
                throw BBL::Error{"exec: argument must be a string"};
            }
            // Script-level exec: fresh isolated scope, returns last expression
            BblLexer lexer(codeVal.stringVal->data.c_str());
            auto nodes = parse(lexer);
            BblScope execScope;
            BblValue result = BblValue::makeNull();
            for (auto& n : nodes) {
                result = eval(n, execScope);
            }
            return result;
        }

        if (op == "execfile") {
            if (node.children.size() < 2) {
                throw BBL::Error{"execfile requires a path argument"};
            }
            BblValue pathVal = eval(node.children[1], scope);
            if (pathVal.type != BBL::Type::String) {
                throw BBL::Error{"execfile: argument must be a string"};
            }
            execfile(pathVal.stringVal->data);
            return BblValue::makeNull();
        }

        // Not a special form - check for built-in operators and functions

        if (op == "not") {
            if (node.children.size() != 2) {
                throw BBL::Error{"not requires exactly 1 argument"};
            }
            BblValue arg = eval(node.children[1], scope);
            if (arg.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: not requires bool, got " + typeName(arg.type)};
            }
            return BblValue::makeBool(!arg.boolVal);
        }

        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            if (node.children.size() < 3) {
                throw BBL::Error{op + " requires at least 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            if (op == "+" && left.type == BBL::Type::String) {
                std::string result = left.stringVal->data;
                for (size_t i = 2; i < node.children.size(); i++) {
                    BblValue right = eval(node.children[i], scope);
                    if (right.type != BBL::Type::String) {
                        throw BBL::Error{"type mismatch: + cannot apply to string and " + typeName(right.type)};
                    }
                    result += right.stringVal->data;
                }
                return BblValue::makeString(intern(result));
            }

            BblValue right = eval(node.children[2], scope);
            if (left.type != BBL::Type::Int && left.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type)};
            }
            if (right.type != BBL::Type::Int && right.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type) + " and " + typeName(right.type)};
            }

            bool useFloat = (left.type == BBL::Type::Float || right.type == BBL::Type::Float);
            if (useFloat) {
                double l = (left.type == BBL::Type::Float) ? left.floatVal : static_cast<double>(left.intVal);
                double r = (right.type == BBL::Type::Float) ? right.floatVal : static_cast<double>(right.intVal);
                if (op == "+") { return BblValue::makeFloat(l + r); }
                if (op == "-") { return BblValue::makeFloat(l - r); }
                if (op == "*") { return BblValue::makeFloat(l * r); }
                if (op == "/") {
                    if (r == 0.0) { throw BBL::Error{"division by zero"}; }
                    return BblValue::makeFloat(l / r);
                }
                if (op == "%") {
                    if (r == 0.0) { throw BBL::Error{"division by zero"}; }
                    return BblValue::makeFloat(std::fmod(l, r));
                }
            } else {
                int64_t l = left.intVal;
                int64_t r = right.intVal;
                if (op == "+") { return BblValue::makeInt(l + r); }
                if (op == "-") { return BblValue::makeInt(l - r); }
                if (op == "*") { return BblValue::makeInt(l * r); }
                if (op == "/") {
                    if (r == 0) { throw BBL::Error{"division by zero"}; }
                    return BblValue::makeInt(l / r);
                }
                if (op == "%") {
                    if (r == 0) { throw BBL::Error{"division by zero"}; }
                    return BblValue::makeInt(l % r);
                }
            }
        }

        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (node.children.size() != 3) {
                throw BBL::Error{op + " requires exactly 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            BblValue right = eval(node.children[2], scope);

            if (op == "==" || op == "!=") {
                bool eq = (left == right);
                return BblValue::makeBool(op == "==" ? eq : !eq);
            }

            if (left.type != BBL::Type::Int && left.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type)};
            }
            if (right.type != BBL::Type::Int && right.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type) + " and " + typeName(right.type)};
            }

            double l = (left.type == BBL::Type::Float) ? left.floatVal : static_cast<double>(left.intVal);
            double r = (right.type == BBL::Type::Float) ? right.floatVal : static_cast<double>(right.intVal);

            if (op == "<")  { return BblValue::makeBool(l < r); }
            if (op == ">")  { return BblValue::makeBool(l > r); }
            if (op == "<=") { return BblValue::makeBool(l <= r); }
            if (op == ">=") { return BblValue::makeBool(l >= r); }
        }

        // Fall through to function call
    }

    // Function call: evaluate head and args
    BblValue headVal = eval(head, scope);

    // fn call
    if (headVal.type == BBL::Type::Fn) {
        std::vector<BblValue> args;
        for (size_t i = 1; i < node.children.size(); i++) {
            args.push_back(eval(node.children[i], scope));
        }
        if (headVal.isCFn) {
            callArgs = std::move(args);
            hasReturn = false;
            returnValue = BblValue::makeNull();
            callStack.push_back(Frame{currentFile, node.line, head.type == NodeType::Symbol ? head.stringVal : "<expr>"});
            int rc = headVal.cfnVal(this);
            callStack.pop_back();
            BblValue ret = hasReturn ? returnValue : BblValue::makeNull();
            callArgs.clear();
            hasReturn = false;
            returnValue = BblValue::makeNull();
            (void)rc;
            return ret;
        }
        return callFn(headVal.fnVal, args, node.line);
    }

    throw BBL::Error{"cannot call " + typeName(headVal.type) + " as a function"};
}

BblValue BblState::callFn(BblFn* fn, const std::vector<BblValue>& args, int callLine) {
    if (args.size() != fn->params.size()) {
        throw BBL::Error{"arity mismatch: expected " + std::to_string(fn->params.size())
                         + " argument(s), got " + std::to_string(args.size())};
    }

    // Fresh scope for the call — no parent scope chain for closures
    // Instead, use captures + args
    BblScope callScope;
    // Load captures
    for (auto& [name, val] : fn->captures) {
        callScope.bindings[name] = val;
    }
    // Bind args (overrides captures if same name)
    for (size_t i = 0; i < fn->params.size(); i++) {
        callScope.bindings[fn->params[i]] = args[i];
    }

    BblValue result = BblValue::makeNull();
    for (auto& node : fn->body) {
        result = eval(node, callScope);
    }
    return result;
}

// ---------- exec / execfile ----------

void BblState::exec(const std::string& source) {
    BblLexer lexer(source.c_str());
    auto nodes = parse(lexer);
    for (auto& node : nodes) {
        eval(node, rootScope);
    }
}

void BblState::execfile(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path resolved;
    if (fs::path(path).is_absolute()) {
        throw BBL::Error{"execfile: absolute paths not allowed: " + path};
    }
    if (path.find("..") != std::string::npos) {
        throw BBL::Error{"execfile: parent directory traversal not allowed: " + path};
    }
    if (scriptDir.empty()) {
        resolved = fs::path(path);
    } else {
        resolved = fs::path(scriptDir) / path;
    }
    std::ifstream file(resolved);
    if (!file.is_open()) {
        throw BBL::Error{"file read failed: " + resolved.string()};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string savedFile = currentFile;
    std::string savedDir = scriptDir;
    currentFile = resolved.string();
    scriptDir = resolved.parent_path().string();
    exec(ss.str());
    currentFile = savedFile;
    scriptDir = savedDir;
}

// ---------- Introspection ----------

bool BblState::has(const std::string& name) const {
    return rootScope.bindings.count(name) > 0;
}

BBL::Type BblState::getType(const std::string& name) const {
    auto it = rootScope.bindings.find(name);
    if (it == rootScope.bindings.end()) {
        return BBL::Type::Null;
    }
    return it->second.type;
}

BblValue BblState::get(const std::string& name) const {
    auto it = rootScope.bindings.find(name);
    if (it == rootScope.bindings.end()) {
        throw BBL::Error{"undefined symbol: " + name};
    }
    return it->second;
}

int64_t BblState::getInt(const std::string& name) const {
    BblValue v = get(name);
    if (v.type != BBL::Type::Int) {
        throw BBL::Error{"type mismatch: expected int, got " + typeName(v.type)};
    }
    return v.intVal;
}

double BblState::getFloat(const std::string& name) const {
    BblValue v = get(name);
    if (v.type != BBL::Type::Float) {
        throw BBL::Error{"type mismatch: expected float, got " + typeName(v.type)};
    }
    return v.floatVal;
}

bool BblState::getBool(const std::string& name) const {
    BblValue v = get(name);
    if (v.type != BBL::Type::Bool) {
        throw BBL::Error{"type mismatch: expected bool, got " + typeName(v.type)};
    }
    return v.boolVal;
}

const char* BblState::getString(const std::string& name) const {
    BblValue v = get(name);
    if (v.type != BBL::Type::String) {
        throw BBL::Error{"type mismatch: expected string, got " + typeName(v.type)};
    }
    return v.stringVal->data.c_str();
}

// ---------- Setters ----------

void BblState::setInt(const std::string& name, int64_t val) {
    rootScope.def(name, BblValue::makeInt(val));
}

void BblState::setFloat(const std::string& name, double val) {
    rootScope.def(name, BblValue::makeFloat(val));
}

void BblState::setString(const std::string& name, const char* str) {
    rootScope.def(name, BblValue::makeString(intern(str)));
}

void BblState::set(const std::string& name, BblValue val) {
    rootScope.def(name, val);
}

// ---------- C function registration ----------

void BblState::defn(const std::string& name, BblCFunction fn) {
    rootScope.def(name, BblValue::makeCFn(fn));
}

// ---------- C function args ----------

int BblState::argCount() const {
    return static_cast<int>(callArgs.size());
}

bool BblState::hasArg(int i) const {
    return i >= 0 && i < static_cast<int>(callArgs.size());
}

BBL::Type BblState::getArgType(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds (count " + std::to_string(argCount()) + ")"};
    }
    return callArgs[i].type;
}

int64_t BblState::getIntArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type != BBL::Type::Int) {
        throw BBL::Error{"type mismatch: expected int arg, got " + typeName(callArgs[i].type)};
    }
    return callArgs[i].intVal;
}

double BblState::getFloatArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type != BBL::Type::Float) {
        throw BBL::Error{"type mismatch: expected float arg, got " + typeName(callArgs[i].type)};
    }
    return callArgs[i].floatVal;
}

bool BblState::getBoolArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type != BBL::Type::Bool) {
        throw BBL::Error{"type mismatch: expected bool arg, got " + typeName(callArgs[i].type)};
    }
    return callArgs[i].boolVal;
}

const char* BblState::getStringArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type != BBL::Type::String) {
        throw BBL::Error{"type mismatch: expected string arg, got " + typeName(callArgs[i].type)};
    }
    return callArgs[i].stringVal->data.c_str();
}

BblBinary* BblState::getBinaryArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type != BBL::Type::Binary) {
        throw BBL::Error{"type mismatch: expected binary arg, got " + typeName(callArgs[i].type)};
    }
    return callArgs[i].binaryVal;
}

BblValue BblState::getArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    return callArgs[i];
}

// ---------- C function return ----------

void BblState::pushInt(int64_t val) {
    returnValue = BblValue::makeInt(val);
    hasReturn = true;
}

void BblState::pushFloat(double val) {
    returnValue = BblValue::makeFloat(val);
    hasReturn = true;
}

void BblState::pushBool(bool val) {
    returnValue = BblValue::makeBool(val);
    hasReturn = true;
}

void BblState::pushString(const char* str) {
    returnValue = BblValue::makeString(intern(str));
    hasReturn = true;
}

void BblState::pushNull() {
    returnValue = BblValue::makeNull();
    hasReturn = true;
}

void BblState::pushBinary(const uint8_t* ptr, size_t size) {
    std::vector<uint8_t> data(ptr, ptr + size);
    returnValue = BblValue::makeBinary(allocBinary(std::move(data)));
    hasReturn = true;
}

// ---------- Backtrace ----------

void BblState::printBacktrace(const std::string& what) {
    fprintf(stderr, "error: %s\n", what.c_str());
    for (int i = static_cast<int>(callStack.size()) - 1; i >= 0; i--) {
        auto& f = callStack[i];
        fprintf(stderr, "  at %s  %s:%d\n", f.expr.c_str(), f.file.c_str(), f.line);
    }
}

// ---------- addPrint / addStdLib ----------

static int bblPrint(BblState* bbl) {
    int count = bbl->argCount();
    for (int i = 0; i < count; i++) {
        BBL::Type t = bbl->getArgType(i);
        char buf[64];
        switch (t) {
            case BBL::Type::String:
                if (bbl->printCapture) {
                    *bbl->printCapture += bbl->getStringArg(i);
                } else {
                    fputs(bbl->getStringArg(i), stdout);
                }
                break;
            case BBL::Type::Int:
                snprintf(buf, sizeof(buf), "%" PRId64, bbl->getIntArg(i));
                if (bbl->printCapture) {
                    *bbl->printCapture += buf;
                } else {
                    fputs(buf, stdout);
                }
                break;
            case BBL::Type::Float:
                snprintf(buf, sizeof(buf), "%g", bbl->getArg(i).floatVal);
                if (bbl->printCapture) {
                    *bbl->printCapture += buf;
                } else {
                    fputs(buf, stdout);
                }
                break;
            case BBL::Type::Bool:
                if (bbl->printCapture) {
                    *bbl->printCapture += bbl->getBoolArg(i) ? "true" : "false";
                } else {
                    fputs(bbl->getBoolArg(i) ? "true" : "false", stdout);
                }
                break;
            case BBL::Type::Null:
                if (bbl->printCapture) {
                    *bbl->printCapture += "null";
                } else {
                    fputs("null", stdout);
                }
                break;
            case BBL::Type::Binary: {
                auto* b = bbl->getBinaryArg(i);
                snprintf(buf, sizeof(buf), "<binary %zu bytes>", b->length());
                if (bbl->printCapture) {
                    *bbl->printCapture += buf;
                } else {
                    fputs(buf, stdout);
                }
                break;
            }
            case BBL::Type::Fn:
                if (bbl->printCapture) {
                    *bbl->printCapture += "<fn>";
                } else {
                    fputs("<fn>", stdout);
                }
                break;
            default:
                if (bbl->printCapture) {
                    *bbl->printCapture += "<unknown>";
                } else {
                    fputs("<unknown>", stdout);
                }
                break;
        }
    }
    return 0;
}

void BBL::addPrint(BblState& bbl) {
    bbl.defn("print", bblPrint);
}

void BBL::addStdLib(BblState& bbl) {
    BBL::addPrint(bbl);
}
