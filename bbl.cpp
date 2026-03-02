#include "bbl.h"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <glob.h>
#include "compiler.h"
#include "vm.h"
#include "jit.h"

// ---------- BblValue ----------

static bool bblValueKeyEqual(const BblValue& a, const BblValue& b) {
    if (a.type != b.type) return false;
    if (a.type == BBL::Type::String) return a.stringVal == b.stringVal;
    if (a.type == BBL::Type::Int) return a.intVal == b.intVal;
    if (a.type == BBL::Type::Float) return a.floatVal == b.floatVal;
    if (a.type == BBL::Type::Bool) return a.boolVal == b.boolVal;
    return false;
}

// ---------- BblTable ----------

static size_t hashValue(const BblValue& v) {
    switch (v.type) {
        case BBL::Type::Int:    return std::hash<int64_t>{}(v.intVal);
        case BBL::Type::String: return std::hash<const void*>{}(static_cast<const void*>(v.stringVal));
        case BBL::Type::Float: {
            uint64_t bits;
            std::memcpy(&bits, &v.floatVal, 8);
            return std::hash<uint64_t>{}(bits);
        }
        case BBL::Type::Bool:   return v.boolVal ? 1 : 0;
        default:                return 0;
    }
}

static BblTable::Entry* tableFindEntry(BblTable::Entry* buckets, size_t cap, const BblValue& key) {
    size_t idx = hashValue(key) & (cap - 1);
    BblTable::Entry* firstTombstone = nullptr;
    for (size_t i = 0; i < cap; i++) {
        BblTable::Entry& e = buckets[(idx + i) & (cap - 1)];
        if (!e.occupied && !e.tombstone) {
            return firstTombstone ? firstTombstone : &e;
        }
        if (e.tombstone) {
            if (!firstTombstone) firstTombstone = &e;
            continue;
        }
        if (bblValueKeyEqual(e.key, key)) return &e;
    }
    return firstTombstone;
}

static void tableGrow(BblTable* tbl) {
    size_t newCap = tbl->capacity < 8 ? 8 : tbl->capacity * 2;
    auto* newBuckets = new BblTable::Entry[newCap];
    for (size_t i = 0; i < tbl->capacity; i++) {
        auto& e = tbl->buckets[i];
        if (e.occupied && !e.tombstone) {
            auto* dest = tableFindEntry(newBuckets, newCap, e.key);
            dest->key = e.key;
            dest->val = e.val;
            dest->occupied = true;
        }
    }
    delete[] tbl->buckets;
    tbl->buckets = newBuckets;
    tbl->capacity = newCap;
}

std::expected<BblValue, BBL::GetError> BblTable::get(const BblValue& key) const {
    if (count == 0) return std::unexpected(BBL::GetError::NotFound);
    auto* e = tableFindEntry(buckets, capacity, key);
    if (!e || !e->occupied || e->tombstone) return std::unexpected(BBL::GetError::NotFound);
    return e->val;
}

void BblTable::set(const BblValue& key, const BblValue& val) {
    if (capacity == 0 || count + 1 > capacity * 3 / 4) tableGrow(this);
    auto* e = tableFindEntry(buckets, capacity, key);
    bool isNew = !e->occupied || e->tombstone;
    e->key = key;
    e->val = val;
    e->occupied = true;
    e->tombstone = false;
    if (isNew) {
        count++;
        order.push_back(key);
    }
    if (key.type == BBL::Type::Int && key.intVal >= nextIntKey)
        nextIntKey = key.intVal + 1;
}

bool BblTable::has(const BblValue& key) const {
    if (count == 0) return false;
    auto* e = tableFindEntry(buckets, capacity, key);
    return e && e->occupied && !e->tombstone;
}

bool BblTable::del(const BblValue& key) {
    if (count == 0) return false;
    auto* e = tableFindEntry(buckets, capacity, key);
    if (!e || !e->occupied || e->tombstone) return false;
    e->tombstone = true;
    e->key = BblValue::makeNull();
    e->val = BblValue::makeNull();
    count--;
    for (auto it = order.begin(); it != order.end(); ++it) {
        if (bblValueKeyEqual(*it, key)) { order.erase(it); break; }
    }
    return true;
}

// ---------- BblValue eq ----------

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
        case BBL::Type::Fn:
            if (isCFn != o.isCFn) return false;
            if (isClosure != o.isClosure) return false;
            if (isCFn) return cfnVal == o.cfnVal;
            if (isClosure) return closureVal == o.closureVal;
            return fnVal == o.fnVal;
        case BBL::Type::Struct:
            return structVal == o.structVal ||
                   (structVal->desc == o.structVal->desc && structVal->data == o.structVal->data);
        case BBL::Type::Vector: return vectorVal == o.vectorVal;
        case BBL::Type::Table:  return tableVal == o.tableVal;
        case BBL::Type::UserData: return userdataVal == o.userdataVal;
        default:                return false;
    }
}

// ---------- BblScope ----------

void BblScope::def(uint32_t id, BblValue val) {
    if (slotMap) {
        auto it = slotMap->find(id);
        if (it != slotMap->end()) { slots[it->second] = val; return; }
    }
    if (!bindings) bindings = std::make_unique<std::unordered_map<uint32_t, BblValue>>();
    (*bindings)[id] = val;
}

void BblScope::set(uint32_t id, BblValue val) {
    if (slotMap) {
        auto it = slotMap->find(id);
        if (it != slotMap->end()) { slots[it->second] = val; return; }
        if (bindings) {
            auto bit = bindings->find(id);
            if (bit != bindings->end()) { bit->second = val; return; }
        }
        throw BBL::Error{"undefined symbol"};
    }
    for (BblScope* s = this; s; s = s->parent) {
        if (s->bindings) {
            auto it = s->bindings->find(id);
            if (it != s->bindings->end()) {
                it->second = val;
                return;
            }
        }
    }
    throw BBL::Error{"undefined symbol"};
}

BblValue* BblScope::lookup(uint32_t id) {
    if (slotMap) {
        auto it = slotMap->find(id);
        if (it != slotMap->end()) return &slots[it->second];
        if (bindings) {
            auto bit = bindings->find(id);
            if (bit != bindings->end()) return &bit->second;
        }
        return nullptr;
    }
    for (BblScope* s = this; s; s = s->parent) {
        if (s->bindings) {
            auto it = s->bindings->find(id);
            if (it != s->bindings->end()) {
                return &it->second;
            }
        }
    }
    return nullptr;
}

uint32_t BblState::resolveSymbol(const std::string& name) const {
    auto it = symbolIds.find(name);
    if (it != symbolIds.end()) return it->second;
    uint32_t id = nextSymbolId++;
    symbolIds[name] = id;
    return id;
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
    if (pos == 0 && pos + 1 < len && src[0] == '#' && src[1] == '!') {
        while (pos < len && peek() != '\n') {
            advance();
        }
    }
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
    if (pos < len && peek() == '.' && pos + 1 < len && src[pos + 1] >= '0' && src[pos + 1] <= '9') {
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

    if (c == ':') {
        advance();
        return {TokenType::Colon, 0, 0, false, "", {}, line};
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
            AstNode dot;
            dot.type = NodeType::DotAccess;
            dot.line = next.line;
            if (field.type == TokenType::Symbol) {
                dot.stringVal = std::move(field.stringVal);
            } else if (field.type == TokenType::Int) {
                dot.stringVal = "";           // sentinel: integer-dot
                dot.intVal = field.intVal;
            } else {
                throw BBL::Error{"parse error: expected field name or integer after '.' at line " + std::to_string(field.line)};
            }
            dot.children.push_back(std::move(lhs));
            lhs = std::move(dot);
        } else if (next.type == TokenType::Colon) {
            Token field = lexer.nextToken();
            if (field.type != TokenType::Symbol) {
                throw BBL::Error{"parse error: expected method name after ':' at line " + std::to_string(field.line)};
            }
            AstNode colon;
            colon.type = NodeType::ColonAccess;
            colon.line = next.line;
            colon.stringVal = std::move(field.stringVal);
            colon.children.push_back(std::move(lhs));
            lhs = std::move(colon);
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

BblState::BblState() {
    rootScope.bindings = std::make_unique<std::unordered_map<uint32_t, BblValue>>();
    vm = std::make_unique<VmState>();

    m.length = intern("length"); m.push = intern("push"); m.pop = intern("pop");
    m.clear = intern("clear"); m.at = intern("at"); m.set = intern("set");
    m.get = intern("get"); m.resize = intern("resize"); m.has = intern("has");
    m.del = intern("delete"); m.keys = intern("keys"); m.find = intern("find");
    m.contains = intern("contains"); m.starts_with = intern("starts-with");
    m.ends_with = intern("ends-with"); m.slice = intern("slice");
    m.split = intern("split"); m.replace = intern("replace");
    m.upper = intern("upper"); m.lower = intern("lower"); m.trim = intern("trim");
    m.copy_from = intern("copy-from");
}

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
    for (auto* s : allocatedStructs) {
        delete s;
    }
    for (auto* v : allocatedVectors) {
        delete v;
    }
    for (auto* t : allocatedTables) {
        delete t;
    }
    for (auto* u : allocatedUserDatas) {
        if (u->desc && u->desc->destructor && u->data) {
            u->desc->destructor(u->data);
        }
        delete u;
    }
    for (auto* c : allocatedClosures) {
        delete c;
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
    allocCount++;
    return str;
}

BblString* BblState::allocString(std::string s) {
    auto* str = new BblString{std::move(s)};
    allocatedStrings.push_back(str);
    allocCount++;
    return str;
}

BblBinary* BblState::allocBinary(std::vector<uint8_t> data) {
    auto* b = new BblBinary{std::move(data)};
    allocatedBinaries.push_back(b);
    allocCount++;
    return b;
}

BblFn* BblState::allocFn() {
    auto* f = new BblFn{};
    allocatedFns.push_back(f);
    allocCount++;
    return f;
}

BblStruct* BblState::allocStruct(StructDesc* desc) {
    auto* s = new BblStruct{desc, std::vector<uint8_t>(desc->totalSize, 0)};
    allocatedStructs.push_back(s);
    allocCount++;
    return s;
}

BblVec* BblState::allocVector(const std::string& elemType, BBL::Type elemTypeTag, size_t elemSize) {
    auto* v = new BblVec{elemType, elemTypeTag, elemSize, {}};
    allocatedVectors.push_back(v);
    allocCount++;
    return v;
}

BblTable* BblState::allocTable() {
    auto* t = new BblTable{};
    allocatedTables.push_back(t);
    allocCount++;
    return t;
}

BblUserData* BblState::allocUserData(const std::string& typeName, void* data) {
    auto it = userDataDescs.find(typeName);
    if (it == userDataDescs.end()) {
        throw BBL::Error{"unknown userdata type: " + typeName};
    }
    auto* u = new BblUserData{&it->second, data, false};
    allocatedUserDatas.push_back(u);
    allocCount++;
    return u;
}

// ---------- GC ----------

static void gcMark(BblValue& val);

static void gcMarkScope(BblScope& scope) {
    if (scope.bindings) {
        for (auto& [name, val] : *scope.bindings) {
            gcMark(val);
        }
    }
    for (auto& val : scope.slots) {
        gcMark(val);
    }
}

static void gcMark(BblValue& val) {
    switch (val.type) {
        case BBL::Type::Binary:
            if (val.binaryVal && !val.binaryVal->marked) {
                val.binaryVal->marked = true;
            }
            break;
        case BBL::Type::Fn:
            if (val.isClosure && val.closureVal && !val.closureVal->marked) {
                val.closureVal->marked = true;
                for (auto& cap : val.closureVal->captures) {
                    gcMark(cap);
                }
            } else if (!val.isCFn && !val.isClosure && val.fnVal && !val.fnVal->marked) {
                val.fnVal->marked = true;
                for (auto& [name, cap] : val.fnVal->captures) {
                    gcMark(cap);
                }
            }
            break;
        case BBL::Type::Struct:
            if (val.structVal && !val.structVal->marked) {
                val.structVal->marked = true;
            }
            break;
        case BBL::Type::Vector:
            if (val.vectorVal && !val.vectorVal->marked) {
                val.vectorVal->marked = true;
            }
            break;
        case BBL::Type::Table:
            if (val.tableVal && !val.tableVal->marked) {
                val.tableVal->marked = true;
                for (size_t i = 0; i < val.tableVal->capacity; i++) {
                    auto& e = val.tableVal->buckets[i];
                    if (e.occupied && !e.tombstone) {
                        BblValue km = e.key;
                        BblValue vm = e.val;
                        gcMark(km);
                        gcMark(vm);
                    }
                }
            }
            break;
        case BBL::Type::UserData:
            if (val.userdataVal && !val.userdataVal->marked) {
                val.userdataVal->marked = true;
            }
            break;
        case BBL::Type::String:
            if (val.stringVal && !val.stringVal->marked) {
                val.stringVal->marked = true;
            }
            break;
        default:
            break;
    }
}

void BblState::gc() {
    // Mark phase
    gcMarkScope(rootScope);
    for (auto* scope : activeScopes) {
        gcMarkScope(*scope);
    }
    for (auto& arg : callArgs) {
        gcMark(arg);
    }
    gcMark(returnValue);
    gcMark(lastRecvPayload);

    // Mark VM stack and frame closures
    if (vm) {
        for (BblValue* p = vm->stack.data(); p < vm->stackTop; p++)
            gcMark(*p);
        for (int i = 0; i < vm->frameCount; i++) {
            if (vm->frames[i].closure) {
                BblValue cv = BblValue::makeClosure(vm->frames[i].closure);
                gcMark(cv);
            }
        }
        for (auto& [id, val] : vm->globals)
            gcMark(val);
    }

    // Sweep helper — partition by mark, cleanup + delete unmarked, clear marks
    auto sweepPool = [](auto& pool, auto cleanup) {
        auto mid = std::partition(pool.begin(), pool.end(),
                                  [](auto* obj) { return obj->marked; });
        for (auto it = mid; it != pool.end(); ++it) {
            cleanup(*it);
            delete *it;
        }
        pool.erase(mid, pool.end());
        for (auto* obj : pool) obj->marked = false;
    };
    auto noop = [](auto*) {};

    sweepPool(allocatedBinaries, noop);
    sweepPool(allocatedFns, noop);
    sweepPool(allocatedClosures, noop);
    sweepPool(allocatedStructs, noop);
    sweepPool(allocatedVectors, noop);
    sweepPool(allocatedTables, noop);
    sweepPool(allocatedUserDatas, [](BblUserData* u) {
        if (u->desc && u->desc->destructor && u->data) {
            u->desc->destructor(u->data);
        }
    });
    sweepPool(allocatedStrings, [this](BblString* s) {
        internTable.erase(s->data);
    });

    // Adaptive threshold: next GC when allocations double the surviving set
    size_t liveCount = allocatedBinaries.size() + allocatedFns.size()
                     + allocatedClosures.size()
                     + allocatedStructs.size() + allocatedVectors.size()
                     + allocatedTables.size() + allocatedUserDatas.size()
                     + allocatedStrings.size();
    gcThreshold = std::max<size_t>(256, liveCount * 2);
    allocCount = 0;
}

// ---------- Eval ----------

std::string typeName(BBL::Type t) {
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

// String no-arg method helper — shared between DotAccess and evalList dispatch
static BblValue stringNoArgMethod(BblState& bbl, const std::string& data, const std::string& method) {
    if (method == "length") {
        return BblValue::makeInt(static_cast<int64_t>(data.size()));
    }
    if (method == "upper") {
        std::string r = data;
        std::transform(r.begin(), r.end(), r.begin(), ::toupper);
        return BblValue::makeString(bbl.intern(r));
    }
    if (method == "lower") {
        std::string r = data;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return BblValue::makeString(bbl.intern(r));
    }
    if (method == "trim") {
        const char* ws = " \t\n\r\f\v";
        size_t start = data.find_first_not_of(ws);
        if (start == std::string::npos) return BblValue::makeString(bbl.intern(""));
        size_t end = data.find_last_not_of(ws);
        return BblValue::makeString(bbl.intern(data.substr(start, end - start + 1)));
    }
    if (method == "trim-left") {
        const char* ws = " \t\n\r\f\v";
        size_t start = data.find_first_not_of(ws);
        if (start == std::string::npos) return BblValue::makeString(bbl.intern(""));
        return BblValue::makeString(bbl.intern(data.substr(start)));
    }
    if (method == "trim-right") {
        const char* ws = " \t\n\r\f\v";
        size_t end = data.find_last_not_of(ws);
        if (end == std::string::npos) return BblValue::makeString(bbl.intern(""));
        return BblValue::makeString(bbl.intern(data.substr(0, end + 1)));
    }
    return BblValue{}; // sentinel: type == Null, caller checks
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
            if (!node.symbolId) node.symbolId = resolveSymbol(node.stringVal);
            BblValue* v = scope.lookup(node.symbolId);
            if (!v) {
                throw BBL::Error{"undefined symbol: " + node.stringVal};
            }
            return *v;
        }
        case NodeType::List:
            return evalList(node, scope);
        case NodeType::DotAccess: {
            // DotAccess: children[0] = left, stringVal = field name
            // stringVal empty → integer-dot (index in intVal)
            // Dot is data-only: struct fields, table keys, integer indices
            BblValue left = eval(node.children[0], scope);
            auto& field = node.stringVal;
            if (field.empty()) {
                int64_t idx = node.intVal;
                if (left.type == BBL::Type::Vector) {
                    return readVecElem(left.vectorVal, static_cast<size_t>(idx));
                }
                if (left.type == BBL::Type::Table) {
                    return left.tableVal->get(BblValue::makeInt(idx)).value_or(BblValue::makeNull());
                }
                throw BBL::Error{"integer index not supported on " + typeName(left.type)};
            }
            if (left.type == BBL::Type::Struct) {
                FieldDesc* fd = nullptr;
                for (auto& f : left.structVal->desc->fields) {
                    if (f.name == field) {
                        fd = &f;
                        break;
                    }
                }
                if (!fd) {
                    throw BBL::Error{"struct " + left.structVal->desc->name + " has no field " + field};
                }
                return readField(left.structVal, *fd);
            }
            if (left.type == BBL::Type::Table) {
                // Dot on table is always key lookup — no method-first resolution
                BblValue key = BblValue::makeString(intern(field));
                return left.tableVal->get(key).value_or(BblValue::makeNull());
            }
            throw BBL::Error{typeName(left.type) + " has no fields"};
        }
        case NodeType::ColonAccess:
            throw BBL::Error{"colon method access must be called: (" + node.stringVal + " ...)"};
    }
    throw BBL::Error{"internal: unknown node type"};
}

// Collect free variables from an AST body that are not in the bound set

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
                           std::vector<std::string>& freeVars);

static void gatherFreeVarsBody(const std::vector<AstNode>& body,
                               std::vector<std::string> bound,
                               std::vector<std::string>& freeVars) {
    for (auto& n : body) {
        // If it's a def or = form, add the name to bound AFTER processing value
        if (n.type == NodeType::List && !n.children.empty()
            && n.children[0].type == NodeType::Symbol
            && n.children.size() >= 3
            && n.children[1].type == NodeType::Symbol) {
            auto& bodyOp = n.children[0].stringVal;
            if (bodyOp == "=") {
                // = name might reference an outer variable — mark as potential free var
                auto& eName = n.children[1].stringVal;
                if (!contains(bound, eName) && !contains(freeVars, eName)) {
                    freeVars.push_back(eName);
                }
                gatherFreeVars(n.children[2], bound, freeVars);
                bound.push_back(n.children[1].stringVal);
                continue;
            }
        }
        gatherFreeVars(n, bound, freeVars);
    }
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
                if (op == "=") {
                    // = is assign-or-create: process all children (name may be free)
                    for (size_t i = 1; i < node.children.size(); i++) {
                        gatherFreeVars(node.children[i], bound, freeVars);
                    }
                    return;
                }
                if (op == "each") {
                    // (each idx container body...)
                    // Container is evaluated before the index binding exists
                    if (node.children.size() >= 3) {
                        gatherFreeVars(node.children[2], bound, freeVars);
                    }
                    // Note: use gatherFreeVars (not gatherFreeVarsBody) because each runs
                    // in the enclosing scope — body assignments must remain capturable.
                    std::vector<std::string> inner = bound;
                    if (node.children.size() >= 2 && node.children[1].type == NodeType::Symbol) {
                        inner.push_back(node.children[1].stringVal);
                    }
                    for (size_t i = 3; i < node.children.size(); i++) {
                        gatherFreeVars(node.children[i], inner, freeVars);
                    }
                    return;
                }
                if (op == "struct") {
                    // struct children are declarative (type/field symbols), not variable references
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
        case NodeType::ColonAccess:
            for (auto& c : node.children) {
                gatherFreeVars(c, bound, freeVars);
            }
            break;
        default:
            break;
    }
}

// Special form dispatch: enum + static hash map for O(1) lookup
enum class SpecialForm {
    None, Eq, If, Loop, Each, And, Or, Fn, Exec, ExecFile,
    Vector, Table, Not, Add, Sub, Mul, Div, Mod,
    CmpEq, CmpNe, CmpLt, CmpGt, CmpLe, CmpGe,
    Do, Band, Bor, Bxor, Bnot, Shl, Shr, With, Break, Continue, Try,
    Struct, Sizeof, Binary
};

static SpecialForm lookupSpecialForm(const std::string& op) {
    static const std::unordered_map<std::string, SpecialForm> table = {
        {"=", SpecialForm::Eq},
        {"if", SpecialForm::If}, {"loop", SpecialForm::Loop}, {"each", SpecialForm::Each},
        {"and", SpecialForm::And}, {"or", SpecialForm::Or}, {"fn", SpecialForm::Fn},
        {"exec", SpecialForm::Exec}, {"execfile", SpecialForm::ExecFile},
        {"vector", SpecialForm::Vector}, {"table", SpecialForm::Table},
        {"not", SpecialForm::Not}, {"do", SpecialForm::Do},
        {"+", SpecialForm::Add}, {"-", SpecialForm::Sub}, {"*", SpecialForm::Mul},
        {"/", SpecialForm::Div}, {"%", SpecialForm::Mod},
        {"==", SpecialForm::CmpEq}, {"!=", SpecialForm::CmpNe},
        {"<", SpecialForm::CmpLt}, {">", SpecialForm::CmpGt},
        {"<=", SpecialForm::CmpLe}, {">=", SpecialForm::CmpGe},
        {"band", SpecialForm::Band}, {"bor", SpecialForm::Bor}, {"bxor", SpecialForm::Bxor},
        {"bnot", SpecialForm::Bnot}, {"shl", SpecialForm::Shl}, {"shr", SpecialForm::Shr},
        {"with", SpecialForm::With},
        {"break", SpecialForm::Break}, {"continue", SpecialForm::Continue},
        {"try", SpecialForm::Try},
        {"struct", SpecialForm::Struct}, {"sizeof", SpecialForm::Sizeof},
        {"binary", SpecialForm::Binary},
    };
    auto it = table.find(op);
    return it != table.end() ? it->second : SpecialForm::None;
}

// ---------- Tail Call Optimization ----------

struct TailCall {
    BblValue args[8];
    std::vector<BblValue> heapArgs;
    size_t argc;
};

static void markTailPosition(AstNode& node, const std::string& fnName) {
    if (node.type != NodeType::List || node.children.empty()) return;
    auto& head = node.children[0];
    // Direct self-call in tail position
    if (head.type == NodeType::Symbol && head.stringVal == fnName) {
        node.isTailCall = true;
        return;
    }
    // Recurse into transparent forms
    if (head.type != NodeType::Symbol) return;
    SpecialForm sf = lookupSpecialForm(head.stringVal);
    switch (sf) {
        case SpecialForm::If:
            if (node.children.size() >= 3) markTailPosition(node.children[2], fnName);
            if (node.children.size() >= 4) markTailPosition(node.children[3], fnName);
            break;
        case SpecialForm::Do:
            if (node.children.size() >= 2) markTailPosition(node.children.back(), fnName);
            break;
        case SpecialForm::Try:
            if (node.children.size() >= 3) markTailPosition(node.children[node.children.size() - 2], fnName);
            break;
        default:
            break;
    }
}

static void markTailCalls(std::vector<AstNode>& body, const std::string& fnName) {
    if (body.empty()) return;
    markTailPosition(body.back(), fnName);
}

static std::string valueToString(const BblValue& val);

BblValue BblState::evalList(const AstNode& node, BblScope& scope) {
    if (node.children.empty()) {
        return BblValue::makeNull();
    }

    auto& head = node.children[0];

    // Special forms
    if (head.type == NodeType::Symbol) {
        auto& op = head.stringVal;
        SpecialForm sf;
        if (head.cachedSpecialForm >= 0) {
            sf = static_cast<SpecialForm>(head.cachedSpecialForm);
        } else {
            sf = lookupSpecialForm(op);
            head.cachedSpecialForm = static_cast<int8_t>(sf);
        }

        switch (sf) {
        case SpecialForm::Eq: {
            if (node.children.size() < 3) {
                throw BBL::Error{"= requires a target and value"};
            }
            auto& target = node.children[1];
            // Place expression: (= v.x val)
            if (target.type == NodeType::DotAccess && target.children.size() == 1) {
                BblValue obj = eval(target.children[0], scope);
                auto& fieldName = target.stringVal;
                BblValue val = eval(node.children[2], scope);
                if (fieldName.empty()) {
                    int64_t idx = target.intVal;
                    if (obj.type == BBL::Type::Vector) {
                        writeVecElem(obj.vectorVal, static_cast<size_t>(idx), val);
                        return BblValue::makeNull();
                    }
                    if (obj.type == BBL::Type::Table) {
                        obj.tableVal->set(BblValue::makeInt(idx), val);
                        return BblValue::makeNull();
                    }
                    throw BBL::Error{"cannot set integer index on " + typeName(obj.type)};
                }
                if (obj.type == BBL::Type::Struct) {
                    FieldDesc* fd = nullptr;
                    for (auto& f : obj.structVal->desc->fields) {
                        if (f.name == fieldName) {
                            fd = &f;
                            break;
                        }
                    }
                    if (!fd) {
                        throw BBL::Error{"struct " + obj.structVal->desc->name + " has no field " + fieldName};
                    }
                    writeField(obj.structVal, *fd, val);
                    return BblValue::makeNull();
                }
                if (obj.type == BBL::Type::Table) {
                    BblValue key = BblValue::makeString(intern(fieldName));
                    obj.tableVal->set(key, val);
                    return BblValue::makeNull();
                }
                throw BBL::Error{"cannot set field on " + typeName(obj.type)};
            }
            if (target.type != NodeType::Symbol) {
                throw BBL::Error{"=: first argument must be a symbol or place expression"};
            }
            BblValue val = eval(node.children[2], scope);
            if (val.type == BBL::Type::Fn && !val.isCFn && val.fnVal) {
                markTailCalls(val.fnVal->body, target.stringVal);
            }
            if (!target.symbolId) target.symbolId = resolveSymbol(target.stringVal);
            uint32_t targetId = target.symbolId;
            // assign-or-create
            BblValue* existing = scope.lookup(targetId);
            if (existing) {
                scope.set(targetId, val);
            } else {
                if (val.type == BBL::Type::Struct) {
                    BblStruct* copy = allocStruct(val.structVal->desc);
                    memcpy(copy->data.data(), val.structVal->data.data(), val.structVal->desc->totalSize);
                    val = BblValue::makeStruct(copy);
                }
                scope.def(targetId, val);
            }
            // Recursive function self-capture (BBL functions only — C functions have no captures)
            if (val.type == BBL::Type::Fn && !val.isCFn && val.fnVal) {
                auto& defName = target.stringVal;
                uint32_t defId = targetId;
                bool alreadyCaptured = false;
                for (auto& [cid, cval] : val.fnVal->captures) {
                    if (cid == defId) { alreadyCaptured = true; break; }
                }
                if (!alreadyCaptured) {
                    std::vector<std::string> freeVars;
                    std::vector<std::string> bound = val.fnVal->params;
                    gatherFreeVarsBody(val.fnVal->body, bound, freeVars);
                    if (contains(freeVars, defName)) {
                        size_t slot = val.fnVal->slotIndex.size();
                        val.fnVal->slotIndex[defId] = slot;
                        val.fnVal->captures.emplace_back(defId, val);
                    }
                }
            }
            return BblValue::makeNull();
        }
        case SpecialForm::Do: {
            BblValue result = BblValue::makeNull();
            for (size_t i = 1; i < node.children.size(); i++) {
                result = eval(node.children[i], scope);
                checkTerminated();
                checkStepLimit();
                if (flowSignal) break;
            }
            return result;
        }
        case SpecialForm::If: {
            if (node.children.size() < 3) {
                throw BBL::Error{"if requires a condition and then-body"};
            }
            BblValue cond = eval(node.children[1], scope);
            if (cond.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: condition must be bool, got " + typeName(cond.type)};
            }
            if (cond.boolVal) {
                return eval(node.children[2], scope);
            } else if (node.children.size() >= 4) {
                return eval(node.children[3], scope);
            }
            return BblValue::makeNull();
        }
        case SpecialForm::Loop: {
            if (node.children.size() < 3) {
                throw BBL::Error{"loop requires a condition and body"};
            }
            while (true) {
                if (allocCount >= gcThreshold) gc();
                BblValue cond = eval(node.children[1], scope);
                if (cond.type != BBL::Type::Bool) {
                    throw BBL::Error{"type mismatch: condition must be bool, got " + typeName(cond.type)};
                }
                if (!cond.boolVal) {
                    break;
                }
                for (size_t i = 2; i < node.children.size(); i++) {
                    eval(node.children[i], scope);
                    checkTerminated();
                    checkStepLimit();
                    if (flowSignal) break;
                }
                if (flowSignal == FlowBreak) { flowSignal = FlowNone; break; }
                if (flowSignal == FlowContinue) { flowSignal = FlowNone; continue; }
            }
            return BblValue::makeNull();
        }
        case SpecialForm::Each: {
            // (each i container body...)
            if (node.children.size() < 4) {
                throw BBL::Error{"each requires an index variable, container, and body"};
            }
            auto& idxNode = node.children[1];
            if (idxNode.type != NodeType::Symbol) {
                throw BBL::Error{"each: first argument must be a symbol (index variable)"};
            }
            uint32_t idxId = resolveSymbol(idxNode.stringVal);
            BblValue container = eval(node.children[2], scope);
            int64_t len = 0;
            if (container.type == BBL::Type::Vector) {
                len = static_cast<int64_t>(container.vectorVal->length());
            } else if (container.type == BBL::Type::Table) {
                len = static_cast<int64_t>(container.tableVal->length());
            } else {
                throw BBL::Error{"each: container must be a vector or table, got " + typeName(container.type)};
            }
            // Bind index variable (assign-or-create)
            BblValue* existing = scope.lookup(idxId);
            if (existing) {
                *existing = BblValue::makeInt(0);
            } else {
                scope.def(idxId, BblValue::makeInt(0));
            }
            for (int64_t idx = 0; idx < len; idx++) {
                if (allocCount >= gcThreshold) gc();
                BblValue* slot = scope.lookup(idxId);
                *slot = BblValue::makeInt(idx);
                for (size_t i = 3; i < node.children.size(); i++) {
                    eval(node.children[i], scope);
                    checkTerminated();
                    checkStepLimit();
                    if (flowSignal) break;
                }
                if (flowSignal == FlowBreak) { flowSignal = FlowNone; break; }
                if (flowSignal == FlowContinue) { flowSignal = FlowNone; continue; }
            }
            // After loop: index = len
            BblValue* slot = scope.lookup(idxId);
            *slot = BblValue::makeInt(len);
            return BblValue::makeNull();
        }
        case SpecialForm::And: {
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
        case SpecialForm::Or: {
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
        case SpecialForm::Fn: {
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
                fn->paramIds.push_back(resolveSymbol(p.stringVal));
            }
            fn->body.assign(node.children.begin() + 2, node.children.end());

            // Capture free variables
            std::vector<std::string> freeVars;
            std::vector<std::string> bound = fn->params;
            gatherFreeVarsBody(fn->body, bound, freeVars);

            // Filter: only capture names that are special forms or builtins
            // Actually, capture everything that exists in scope
            static const std::vector<std::string> specialForms = {
                "=", "if", "loop", "each", "and", "or", "fn", "exec", "not", "do"
            };
            for (auto& name : freeVars) {
                if (contains(specialForms, name)) {
                    continue;
                }
                uint32_t nameId = resolveSymbol(name);
                BblValue* val = scope.lookup(nameId);
                if (val) {
                    fn->captures.emplace_back(nameId, *val);
                }
                // If not found in scope, it might be defined later or be an error at call time
            }

            // Build slot layout: captures first, then params
            {
                size_t slot = 0;
                for (auto& [id, val] : fn->captures) {
                    fn->slotIndex[id] = slot++;
                }
                fn->paramSlotStart = slot;
                for (auto id : fn->paramIds) {
                    fn->slotIndex[id] = slot++;
                }
            }

            return BblValue::makeFn(fn);
        }
        case SpecialForm::Exec: {
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
            activeScopes.push_back(&execScope);
            BblValue result = BblValue::makeNull();
            try {
                for (auto& n : nodes) {
                    result = eval(n, execScope);
                }
            } catch (...) {
                activeScopes.pop_back();
                throw;
            }
            activeScopes.pop_back();
            return result;
        }
        case SpecialForm::ExecFile: {
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
        case SpecialForm::Vector: {
            if (node.children.size() < 2) {
                throw BBL::Error{"vector requires a type argument"};
            }
            auto& typeNode = node.children[1];
            if (typeNode.type != NodeType::Symbol) {
                throw BBL::Error{"vector: first argument must be a type name"};
            }
            auto& elemTypeName = typeNode.stringVal;
            BBL::Type elemTag;
            size_t elemSize;
            if (elemTypeName == "int") {
                elemTag = BBL::Type::Int;
                elemSize = sizeof(int64_t);
            } else if (elemTypeName == "float") {
                elemTag = BBL::Type::Float;
                elemSize = sizeof(double);
            } else if (elemTypeName == "bool") {
                elemTag = BBL::Type::Bool;
                elemSize = 1;
            } else {
                auto sit = structDescs.find(elemTypeName);
                if (sit == structDescs.end()) {
                    throw BBL::Error{"vector: unknown element type " + elemTypeName};
                }
                elemTag = BBL::Type::Struct;
                elemSize = sit->second.totalSize;
            }
            BblVec* vec = allocVector(elemTypeName, elemTag, elemSize);
            // Binary bulk-load: (vector Type binaryVal)
            if (node.children.size() == 3) {
                BblValue arg = eval(node.children[2], scope);
                if (arg.type == BBL::Type::Binary) {
                    if (elemSize == 0 || arg.binaryVal->data.size() % elemSize != 0) {
                        throw BBL::Error{"vector: binary size " + std::to_string(arg.binaryVal->data.size()) +
                                         " is not a multiple of element size " + std::to_string(elemSize)};
                    }
                    vec->data = arg.binaryVal->data;
                    return BblValue::makeVector(vec);
                }
                packValue(vec, arg);
                return BblValue::makeVector(vec);
            }
            for (size_t i = 2; i < node.children.size(); i++) {
                BblValue elem = eval(node.children[i], scope);
                packValue(vec, elem);
            }
            return BblValue::makeVector(vec);
        }
        case SpecialForm::Table: {
            BblTable* tbl = allocTable();
            size_t argCount = node.children.size() - 1;
            if (argCount % 2 != 0) {
                throw BBL::Error{"table requires even number of arguments (key-value pairs)"};
            }
            for (size_t i = 1; i < node.children.size(); i += 2) {
                BblValue key = eval(node.children[i], scope);
                BblValue val = eval(node.children[i + 1], scope);
                if (key.type != BBL::Type::String && key.type != BBL::Type::Int) {
                    throw BBL::Error{"table key must be string or int, got " + typeName(key.type)};
                }
                tbl->set(key, val);
            }
            return BblValue::makeTable(tbl);
        }
        case SpecialForm::Not: {
            if (node.children.size() != 2) {
                throw BBL::Error{"not requires exactly 1 argument"};
            }
            BblValue arg = eval(node.children[1], scope);
            if (arg.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: not requires bool, got " + typeName(arg.type)};
            }
            return BblValue::makeBool(!arg.boolVal);
        }
        case SpecialForm::Add:
        case SpecialForm::Sub:
        case SpecialForm::Mul:
        case SpecialForm::Div:
        case SpecialForm::Mod: {
            if (node.children.size() < 3) {
                throw BBL::Error{op + " requires at least 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            if (op == "+" && left.type == BBL::Type::String) {
                std::string result = left.stringVal->data;
                for (size_t i = 2; i < node.children.size(); i++) {
                    BblValue right = eval(node.children[i], scope);
                    if (right.type == BBL::Type::String) {
                        result += right.stringVal->data;
                    } else {
                        result += valueToString(right);
                    }
                }
                return BblValue::makeString(allocString(std::move(result)));
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
                switch (sf) {
                    case SpecialForm::Add: return BblValue::makeFloat(l + r);
                    case SpecialForm::Sub: return BblValue::makeFloat(l - r);
                    case SpecialForm::Mul: return BblValue::makeFloat(l * r);
                    case SpecialForm::Div:
                        if (r == 0.0) { throw BBL::Error{"division by zero"}; }
                        return BblValue::makeFloat(l / r);
                    case SpecialForm::Mod:
                        if (r == 0.0) { throw BBL::Error{"division by zero"}; }
                        return BblValue::makeFloat(std::fmod(l, r));
                    default: break;
                }
            } else {
                int64_t l = left.intVal;
                int64_t r = right.intVal;
                switch (sf) {
                    case SpecialForm::Add: return BblValue::makeInt(l + r);
                    case SpecialForm::Sub: return BblValue::makeInt(l - r);
                    case SpecialForm::Mul: return BblValue::makeInt(l * r);
                    case SpecialForm::Div:
                        if (r == 0) { throw BBL::Error{"division by zero"}; }
                        return BblValue::makeInt(l / r);
                    case SpecialForm::Mod:
                        if (r == 0) { throw BBL::Error{"division by zero"}; }
                        return BblValue::makeInt(l % r);
                    default: break;
                }
            }
            break; // unreachable but satisfies compiler
        }

        case SpecialForm::CmpEq:
        case SpecialForm::CmpNe:
        case SpecialForm::CmpLt:
        case SpecialForm::CmpGt:
        case SpecialForm::CmpLe:
        case SpecialForm::CmpGe: {
            if (node.children.size() != 3) {
                throw BBL::Error{op + " requires exactly 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            BblValue right = eval(node.children[2], scope);

            if (sf == SpecialForm::CmpEq || sf == SpecialForm::CmpNe) {
                bool eq = (left == right);
                return BblValue::makeBool(sf == SpecialForm::CmpEq ? eq : !eq);
            }

            // String ordering — byte-level lexicographic (no locale)
            if (left.type == BBL::Type::String && right.type == BBL::Type::String) {
                int cmp = left.stringVal->data.compare(right.stringVal->data);
                switch (sf) {
                    case SpecialForm::CmpLt: return BblValue::makeBool(cmp < 0);
                    case SpecialForm::CmpGt: return BblValue::makeBool(cmp > 0);
                    case SpecialForm::CmpLe: return BblValue::makeBool(cmp <= 0);
                    case SpecialForm::CmpGe: return BblValue::makeBool(cmp >= 0);
                    default: break;
                }
            }

            if (left.type != BBL::Type::Int && left.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type)};
            }
            if (right.type != BBL::Type::Int && right.type != BBL::Type::Float) {
                throw BBL::Error{"type mismatch: " + op + " cannot apply to " + typeName(left.type) + " and " + typeName(right.type)};
            }

            // Integer fast-path — skip int→double conversion
            if (left.type == BBL::Type::Int && right.type == BBL::Type::Int) {
                switch (sf) {
                    case SpecialForm::CmpLt: return BblValue::makeBool(left.intVal < right.intVal);
                    case SpecialForm::CmpGt: return BblValue::makeBool(left.intVal > right.intVal);
                    case SpecialForm::CmpLe: return BblValue::makeBool(left.intVal <= right.intVal);
                    case SpecialForm::CmpGe: return BblValue::makeBool(left.intVal >= right.intVal);
                    default: break;
                }
            }

            double l = (left.type == BBL::Type::Float) ? left.floatVal : static_cast<double>(left.intVal);
            double r = (right.type == BBL::Type::Float) ? right.floatVal : static_cast<double>(right.intVal);

            switch (sf) {
                case SpecialForm::CmpLt: return BblValue::makeBool(l < r);
                case SpecialForm::CmpGt: return BblValue::makeBool(l > r);
                case SpecialForm::CmpLe: return BblValue::makeBool(l <= r);
                case SpecialForm::CmpGe: return BblValue::makeBool(l >= r);
                default: break;
            }
            break;
        }

        case SpecialForm::Bnot: {
            if (node.children.size() != 2) {
                throw BBL::Error{"bnot requires exactly 1 argument"};
            }
            BblValue arg = eval(node.children[1], scope);
            if (arg.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: bnot requires int, got " + typeName(arg.type)};
            }
            return BblValue::makeInt(~arg.intVal);
        }

        case SpecialForm::Band:
        case SpecialForm::Bor:
        case SpecialForm::Bxor: {
            if (node.children.size() < 3) {
                throw BBL::Error{op + " requires at least 2 arguments"};
            }
            BblValue first = eval(node.children[1], scope);
            if (first.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: " + op + " requires int, got " + typeName(first.type)};
            }
            int64_t result = first.intVal;
            for (size_t i = 2; i < node.children.size(); i++) {
                BblValue arg = eval(node.children[i], scope);
                if (arg.type != BBL::Type::Int) {
                    throw BBL::Error{"type mismatch: " + op + " requires int, got " + typeName(arg.type)};
                }
                switch (sf) {
                    case SpecialForm::Band: result &= arg.intVal; break;
                    case SpecialForm::Bor:  result |= arg.intVal; break;
                    case SpecialForm::Bxor: result ^= arg.intVal; break;
                    default: break;
                }
            }
            return BblValue::makeInt(result);
        }

        case SpecialForm::Shl:
        case SpecialForm::Shr: {
            if (node.children.size() != 3) {
                throw BBL::Error{op + " requires exactly 2 arguments"};
            }
            BblValue left = eval(node.children[1], scope);
            BblValue right = eval(node.children[2], scope);
            if (left.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: " + op + " requires int, got " + typeName(left.type)};
            }
            if (right.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: " + op + " requires int, got " + typeName(right.type)};
            }
            int64_t val = left.intVal;
            int64_t shift = right.intVal;
            if (shift < 0) {
                throw BBL::Error{op + " requires non-negative shift amount"};
            }
            if (shift >= 64) {
                if (sf == SpecialForm::Shr && val < 0) return BblValue::makeInt(int64_t(-1));
                return BblValue::makeInt(int64_t(0));
            }
            if (sf == SpecialForm::Shl) return BblValue::makeInt(val << shift);
            return BblValue::makeInt(val >> shift);
        }

        case SpecialForm::Break: {
            if (node.children.size() != 1)
                throw BBL::Error{"break takes no arguments"};
            flowSignal = FlowBreak;
            return BblValue::makeNull();
        }
        case SpecialForm::Continue: {
            if (node.children.size() != 1)
                throw BBL::Error{"continue takes no arguments"};
            flowSignal = FlowContinue;
            return BblValue::makeNull();
        }

        case SpecialForm::Try: {
            // (try body... (catch err-var handler...))
            if (node.children.size() < 3)
                throw BBL::Error{"try requires a body and catch clause"};
            auto& catchNode = node.children.back();
            if (catchNode.type != NodeType::List || catchNode.children.empty() ||
                catchNode.children[0].type != NodeType::Symbol ||
                catchNode.children[0].stringVal != "catch")
                throw BBL::Error{"try: last argument must be a (catch err-var handler...) clause"};
            if (catchNode.children.size() < 2)
                throw BBL::Error{"catch requires an error variable name"};
            if (catchNode.children[1].type != NodeType::Symbol)
                throw BBL::Error{"catch: error variable must be a symbol"};

            BblValue result = BblValue::makeNull();
            try {
                for (size_t i = 1; i < node.children.size() - 1; i++) {
                    result = eval(node.children[i], scope);
                    checkTerminated();
                    checkStepLimit();
                    if (flowSignal) return result;
                }
            } catch (BBL::Error& e) {
                BblScope catchScope;
                catchScope.parent = &scope;
                uint32_t errId = resolveSymbol(catchNode.children[1].stringVal);
                catchScope.def(errId, BblValue::makeString(intern(e.what)));
                activeScopes.push_back(&catchScope);
                result = BblValue::makeNull();
                try {
                    for (size_t i = 2; i < catchNode.children.size(); i++) {
                        result = eval(catchNode.children[i], catchScope);
                        if (flowSignal) break;
                    }
                } catch (...) {
                    activeScopes.pop_back();
                    throw;
                }
                activeScopes.pop_back();
            }
            return result;
        }

        case SpecialForm::With: {
            // (with name init body...)
            if (node.children.size() < 3)
                throw BBL::Error{"with requires a name, initializer, and body"};
            if (node.children[1].type != NodeType::Symbol)
                throw BBL::Error{"with: first argument must be a symbol"};
            const std::string& name = node.children[1].stringVal;
            BblValue val = eval(node.children[2], scope);
            if (val.type != BBL::Type::UserData)
                throw BBL::Error{"with: initializer must produce userdata, got " + typeName(val.type)};
            BblUserData* ud = val.userdataVal;
            BblScope withScope;
            withScope.parent = &scope;
            uint32_t nameId = resolveSymbol(name);
            withScope.def(nameId, val);
            activeScopes.push_back(&withScope);

            auto cleanup = [&]() {
                activeScopes.pop_back();
                if (ud->desc && ud->desc->destructor && ud->data) {
                    ud->desc->destructor(ud->data);
                    ud->data = nullptr;
                }
            };

            BblValue result = BblValue::makeNull();
            try {
                for (size_t i = 3; i < node.children.size(); i++) {
                    result = eval(node.children[i], withScope);
                    if (flowSignal) break;
                }
            } catch (...) {
                try { cleanup(); } catch (...) { /* swallow destructor exception */ }
                throw;
            }
            cleanup();
            return result;
        }

        case SpecialForm::Struct: {
            // (struct Name type1 field1 type2 field2 ...)
            if (node.children.size() < 4 || node.children.size() % 2 != 0) {
                throw BBL::Error{"struct: expected (struct Name type field ...)"};
            }
            if (node.children[1].type != NodeType::Symbol) {
                throw BBL::Error{"struct: name must be a symbol"};
            }
            auto& sname = node.children[1].stringVal;
            if (structDescs.find(sname) != structDescs.end()) {
                throw BBL::Error{"struct " + sname + " already defined"};
            }
            // Primitive type lookup table
            struct TypeInfo { CType ct; size_t size; };
            static const std::unordered_map<std::string, TypeInfo> typeTable = {
                {"bool",    {CType::Bool,    1}},
                {"int8",    {CType::Int8,    1}},
                {"uint8",   {CType::Uint8,   1}},
                {"int16",   {CType::Int16,   2}},
                {"uint16",  {CType::Uint16,  2}},
                {"int32",   {CType::Int32,   4}},
                {"uint32",  {CType::Uint32,  4}},
                {"int64",   {CType::Int64,   8}},
                {"uint64",  {CType::Uint64,  8}},
                {"float32", {CType::Float32, 4}},
                {"float64", {CType::Float64, 8}},
            };
            StructDesc desc;
            desc.name = sname;
            size_t offset = 0;
            std::vector<std::string> fieldNames;
            for (size_t i = 2; i < node.children.size(); i += 2) {
                if (node.children[i].type != NodeType::Symbol) {
                    throw BBL::Error{"struct " + sname + ": expected type symbol at position " + std::to_string(i)};
                }
                if (node.children[i+1].type != NodeType::Symbol) {
                    throw BBL::Error{"struct " + sname + ": expected field name symbol at position " + std::to_string(i+1)};
                }
                auto& typeSym = node.children[i].stringVal;
                auto& fieldName = node.children[i+1].stringVal;
                // Check duplicate field names
                for (auto& fn : fieldNames) {
                    if (fn == fieldName) {
                        throw BBL::Error{"struct " + sname + ": duplicate field name " + fieldName};
                    }
                }
                fieldNames.push_back(fieldName);
                auto tit = typeTable.find(typeSym);
                if (tit != typeTable.end()) {
                    desc.fields.push_back(FieldDesc{fieldName, offset, tit->second.size, tit->second.ct, ""});
                    offset += tit->second.size;
                } else {
                    // Check for nested struct type
                    auto sit = structDescs.find(typeSym);
                    if (sit != structDescs.end()) {
                        desc.fields.push_back(FieldDesc{fieldName, offset, sit->second.totalSize, CType::Struct, typeSym});
                        offset += sit->second.totalSize;
                    } else {
                        throw BBL::Error{"struct " + sname + ": unknown type " + typeSym};
                    }
                }
            }
            desc.totalSize = offset;
            structDescs[sname] = std::move(desc);
            return BblValue::makeNull();
        }

        case SpecialForm::Sizeof: {
            // (sizeof Name) or (sizeof expr)
            if (node.children.size() != 2) {
                throw BBL::Error{"sizeof: expected exactly 1 argument"};
            }
            // Check raw AST symbol first (before evaluating)
            if (node.children[1].type == NodeType::Symbol) {
                auto sit = structDescs.find(node.children[1].stringVal);
                if (sit != structDescs.end()) {
                    return BblValue::makeInt(static_cast<int64_t>(sit->second.totalSize));
                }
            }
            // Fall through: evaluate and check if struct value
            BblValue val = eval(node.children[1], scope);
            if (val.type == BBL::Type::Struct) {
                return BblValue::makeInt(static_cast<int64_t>(val.structVal->desc->totalSize));
            }
            throw BBL::Error{"sizeof: argument must be a struct type name or struct value"};
        }

        case SpecialForm::Binary: {
            if (node.children.size() != 2) {
                throw BBL::Error{"binary: expected exactly 1 argument"};
            }
            BblValue arg = eval(node.children[1], scope);
            if (arg.type == BBL::Type::Vector) {
                return BblValue::makeBinary(allocBinary(arg.vectorVal->data));
            }
            if (arg.type == BBL::Type::Struct) {
                return BblValue::makeBinary(allocBinary(arg.structVal->data));
            }
            if (arg.type == BBL::Type::Int) {
                if (arg.intVal < 0) {
                    throw BBL::Error{"binary: size must be non-negative"};
                }
                return BblValue::makeBinary(allocBinary(std::vector<uint8_t>(static_cast<size_t>(arg.intVal), 0)));
            }
            throw BBL::Error{"binary: argument must be vector, struct, or non-negative integer"};
        }

        case SpecialForm::None: {
            // Check if this is a struct constructor call
            auto sit = structDescs.find(op);
            if (sit != structDescs.end()) {
                std::vector<BblValue> args;
                for (size_t i = 1; i < node.children.size(); i++) {
                    args.push_back(eval(node.children[i], scope));
                }
                return constructStruct(&sit->second, args, node.line);
            }
            break; // fall through to function call
        }
        } // end switch(sf)
    }

    // ColonAccess call: (obj:method args...)
    if (head.type == NodeType::ColonAccess && head.children.size() >= 1) {
        BblValue obj = eval(head.children[0], scope);
        auto& method = head.stringVal;

        if (obj.type == BBL::Type::Vector) {
            return evalVectorMethod(obj.vectorVal, method, node, scope);
        }

        if (obj.type == BBL::Type::String) {
            return evalStringMethod(obj.stringVal, method, obj, node, scope);
        }

        if (obj.type == BBL::Type::Binary) {
            return evalBinaryMethod(obj.binaryVal, method, node, scope);
        }

        if (obj.type == BBL::Type::Table) {
            // Check built-in table methods first
            static const std::vector<std::string> builtinMethods = {
                "get", "set", "delete", "has", "keys", "length", "push", "pop", "at"
            };
            bool isBuiltin = false;
            for (auto& m : builtinMethods) {
                if (method == m) { isBuiltin = true; break; }
            }
            if (isBuiltin) {
                return evalTableMethod(obj.tableVal, method, node, scope);
            }
            // Lua-style self-passing sugar: look up method as key, call with table as first arg
            auto funcResult = obj.tableVal->get(BblValue::makeString(intern(method)));
            if (!funcResult || funcResult->type != BBL::Type::Fn) {
                throw BBL::Error{"table has no method " + method + " and key '" + method + "' is not a function"};
            }
            // Evaluate call arguments
            std::vector<BblValue> selfArgs;
            selfArgs.push_back(obj);  // self
            for (size_t i = 1; i < node.children.size(); i++) {
                selfArgs.push_back(eval(node.children[i], scope));
            }
            BblValue funcVal = *funcResult;
            if (funcVal.isCFn) {
                callArgs = std::move(selfArgs);
                hasReturn = false;
                returnValue = BblValue::makeNull();
                callStack.push_back(Frame{currentFile, node.line, method});
                int rc = funcVal.cfnVal(this);
                callStack.pop_back();
                BblValue ret = hasReturn ? returnValue : BblValue::makeNull();
                callArgs.clear();
                hasReturn = false;
                returnValue = BblValue::makeNull();
                (void)rc;
                return ret;
            }
            return callFn(funcVal.fnVal, selfArgs.data(), selfArgs.size(), node.line);
        }

        if (obj.type == BBL::Type::UserData) {
            BblUserData* ud = obj.userdataVal;
            auto mit = ud->desc->methods.find(method);
            if (mit == ud->desc->methods.end()) {
                throw BBL::Error{"userdata " + ud->desc->name + " has no method " + method};
            }
            // Build args: first arg is the userdata itself, then remaining call args
            std::vector<BblValue> args;
            args.push_back(obj);
            for (size_t i = 1; i < node.children.size(); i++) {
                args.push_back(eval(node.children[i], scope));
            }
            callArgs = std::move(args);
            hasReturn = false;
            returnValue = BblValue::makeNull();
            callStack.push_back(Frame{currentFile, node.line, ud->desc->name + ":" + method});
            int rc = mit->second(this);
            callStack.pop_back();
            BblValue ret = hasReturn ? returnValue : BblValue::makeNull();
            callArgs.clear();
            hasReturn = false;
            returnValue = BblValue::makeNull();
            (void)rc;
            return ret;
        }

        throw BBL::Error{typeName(obj.type) + " has no methods"};
    }

    // Function call: evaluate head and args
    BblValue headVal = eval(head, scope);

    // fn call
    if (headVal.type == BBL::Type::Fn) {
        size_t argc = node.children.size() - 1;
        BblValue sbuf[8];
        std::vector<BblValue> heap_args;
        BblValue* args;
        if (argc <= 8) {
            for (size_t i = 0; i < argc; i++) sbuf[i] = eval(node.children[i + 1], scope);
            args = sbuf;
        } else {
            heap_args.resize(argc);
            for (size_t i = 0; i < argc; i++) heap_args[i] = eval(node.children[i + 1], scope);
            args = heap_args.data();
        }
        if (headVal.isCFn) {
            callArgs.assign(args, args + argc);
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
        if (node.isTailCall && headVal.fnVal == currentFn && currentFn != nullptr) {
            if (argc != currentFn->params.size()) {
                throw BBL::Error{"arity mismatch: expected " + std::to_string(currentFn->params.size())
                                 + " argument(s), got " + std::to_string(argc)};
            }
            TailCall tc;
            tc.argc = argc;
            if (argc <= 8) {
                for (size_t i = 0; i < argc; i++) tc.args[i] = args[i];
            } else {
                tc.heapArgs.assign(args, args + argc);
            }
            throw tc;
        }
        return callFn(headVal.fnVal, args, argc, node.line);
    }

    throw BBL::Error{"cannot call " + typeName(headVal.type) + " as a function"};
}

// ---------------------------------------------------------------------------
// Extracted method dispatchers
// ---------------------------------------------------------------------------

static size_t toIndex(int64_t val, size_t length, const char* context);

BblValue BblState::evalBinaryMethod(BblBinary* bin, const std::string& method,
                                    const AstNode& node, BblScope& scope) {
    if (method == "length") {
        return BblValue::makeInt(static_cast<int64_t>(bin->length()));
    }
    if (method == "at") {
        if (node.children.size() < 2) {
            throw BBL::Error{"binary.at requires an index"};
        }
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) {
            throw BBL::Error{"binary.at: index must be int"};
        }
        size_t i = toIndex(idx.intVal, bin->data.size(), "binary.at");
        return BblValue::makeInt(static_cast<int64_t>(bin->data[i]));
    }
    if (method == "set") {
        if (node.children.size() < 3) {
            throw BBL::Error{"binary.set requires an index and a value"};
        }
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) {
            throw BBL::Error{"binary.set: index must be int"};
        }
        BblValue val = eval(node.children[2], scope);
        if (val.type != BBL::Type::Int) {
            throw BBL::Error{"binary.set: value must be int"};
        }
        size_t i = toIndex(idx.intVal, bin->data.size(), "binary.set");
        bin->data[i] = static_cast<uint8_t>(val.intVal);
        return BblValue::makeNull();
    }
    if (method == "slice") {
        if (node.children.size() < 3) {
            throw BBL::Error{"binary.slice requires start and length"};
        }
        BblValue startVal = eval(node.children[1], scope);
        BblValue lenVal = eval(node.children[2], scope);
        if (startVal.type != BBL::Type::Int || lenVal.type != BBL::Type::Int) {
            throw BBL::Error{"binary.slice: start and length must be int"};
        }
        int64_t start = startVal.intVal;
        int64_t len = lenVal.intVal;
        if (start < 0 || len < 0) {
            throw BBL::Error{"binary.slice: start and length must be non-negative"};
        }
        size_t ustart = static_cast<size_t>(start);
        size_t ulen = static_cast<size_t>(len);
        if (ustart > bin->data.size() || ulen > bin->data.size() - ustart) {
            throw BBL::Error{"binary.slice: range [" + std::to_string(start) + ", " +
                             std::to_string(start + len) + ") exceeds length " +
                             std::to_string(bin->data.size())};
        }
        auto beg = bin->data.begin() + static_cast<ptrdiff_t>(ustart);
        return BblValue::makeBinary(allocBinary(std::vector<uint8_t>(beg, beg + static_cast<ptrdiff_t>(ulen))));
    }
    if (method == "resize") {
        if (node.children.size() < 2) {
            throw BBL::Error{"binary.resize requires a size"};
        }
        BblValue sizeVal = eval(node.children[1], scope);
        if (sizeVal.type != BBL::Type::Int) {
            throw BBL::Error{"binary.resize: size must be int"};
        }
        if (sizeVal.intVal < 0) {
            throw BBL::Error{"binary.resize: size must be non-negative"};
        }
        bin->data.resize(static_cast<size_t>(sizeVal.intVal), 0);
        return BblValue::makeNull();
    }
    if (method == "copy-from") {
        if (node.children.size() < 2 || node.children.size() > 3) {
            throw BBL::Error{"binary.copy-from requires 1 or 2 arguments"};
        }
        BblValue srcVal = eval(node.children[1], scope);
        if (srcVal.type != BBL::Type::Binary) {
            throw BBL::Error{"binary.copy-from: source must be binary"};
        }
        int64_t offset = 0;
        if (node.children.size() == 3) {
            BblValue offVal = eval(node.children[2], scope);
            if (offVal.type != BBL::Type::Int) {
                throw BBL::Error{"binary.copy-from: offset must be int"};
            }
            offset = offVal.intVal;
        }
        if (offset < 0) {
            throw BBL::Error{"binary.copy-from: offset must be non-negative"};
        }
        BblBinary* src = srcVal.binaryVal;
        if (static_cast<size_t>(offset) + src->data.size() > bin->data.size()) {
            throw BBL::Error{"binary.copy-from: source (" + std::to_string(src->data.size()) +
                             " bytes) at offset " + std::to_string(offset) +
                             " exceeds destination length " + std::to_string(bin->data.size())};
        }
        std::memcpy(bin->data.data() + offset, src->data.data(), src->data.size());
        return BblValue::makeNull();
    }
    throw BBL::Error{"binary has no method " + method};
}

BblValue BblState::evalVectorMethod(BblVec* vec, const std::string& method,
                                    const AstNode& node, BblScope& scope) {
    if (method == "length") {
        return BblValue::makeInt(static_cast<int64_t>(vec->length()));
    }
    if (method == "push") {
        if (node.children.size() < 2) {
            throw BBL::Error{"vector.push requires an argument"};
        }
        BblValue val = eval(node.children[1], scope);
        packValue(vec, val);
        return BblValue::makeNull();
    }
    if (method == "pop") {
        if (vec->length() == 0) {
            throw BBL::Error{"vector.pop on empty vector"};
        }
        BblValue val = readVecElem(vec, vec->length() - 1);
        vec->data.resize(vec->data.size() - vec->elemSize);
        return val;
    }
    if (method == "clear") {
        vec->data.clear();
        return BblValue::makeNull();
    }
    if (method == "at") {
        if (node.children.size() < 2) {
            throw BBL::Error{"vector.at requires an index"};
        }
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) {
            throw BBL::Error{"vector.at: index must be int"};
        }
        return readVecElem(vec, toIndex(idx.intVal, vec->length(), "vector.at"));
    }
    if (method == "set") {
        if (node.children.size() < 3) {
            throw BBL::Error{"vector.set requires an index and a value"};
        }
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) {
            throw BBL::Error{"vector.set: index must be int"};
        }
        BblValue val = eval(node.children[2], scope);
        writeVecElem(vec, toIndex(idx.intVal, vec->length(), "vector.set"), val);
        return BblValue::makeNull();
    }
    if (method == "resize") {
        if (node.children.size() < 2)
            throw BBL::Error{"vector.resize requires a size"};
        BblValue sizeVal = eval(node.children[1], scope);
        if (sizeVal.type != BBL::Type::Int)
            throw BBL::Error{"vector.resize: size must be int"};
        int64_t n = sizeVal.intVal;
        if (n < 0) throw BBL::Error{"vector.resize: size must be non-negative"};
        vec->data.resize(static_cast<size_t>(n) * vec->elemSize, 0);
        return BblValue::makeNull();
    }
    if (method == "reserve") {
        if (node.children.size() < 2)
            throw BBL::Error{"vector.reserve requires a capacity"};
        BblValue capVal = eval(node.children[1], scope);
        if (capVal.type != BBL::Type::Int)
            throw BBL::Error{"vector.reserve: capacity must be int"};
        int64_t n = capVal.intVal;
        if (n < 0) throw BBL::Error{"vector.reserve: capacity must be non-negative"};
        vec->data.reserve(static_cast<size_t>(n) * vec->elemSize);
        return BblValue::makeNull();
    }
    throw BBL::Error{"vector has no method " + method};
}

BblValue BblState::evalStringMethod(BblString* strObj, const std::string& method,
                                    const BblValue& obj, const AstNode& node, BblScope& scope) {
    const std::string& data = strObj->data;

    // No-arg methods (also work in DotAccess)
    BblValue noarg = stringNoArgMethod(*this, data, method);
    if (noarg.type != BBL::Type::Null) return noarg;

    // at
    if (method == "at") {
        if (node.children.size() < 2) throw BBL::Error{"string.at requires 1 argument"};
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) throw BBL::Error{"string.at: index must be int"};
        int64_t i = idx.intVal;
        if (i < 0 || i >= static_cast<int64_t>(data.size())) {
            throw BBL::Error{"string.at: index " + std::to_string(i) + " out of bounds (length " + std::to_string(data.size()) + ")"};
        }
        return BblValue::makeString(intern(std::string(1, data[static_cast<size_t>(i)])));
    }

    // slice
    if (method == "slice") {
        if (node.children.size() < 2) throw BBL::Error{"string.slice requires at least 1 argument"};
        BblValue startVal = eval(node.children[1], scope);
        if (startVal.type != BBL::Type::Int) throw BBL::Error{"string.slice: start must be int"};
        int64_t start = startVal.intVal;
        int64_t end = static_cast<int64_t>(data.size());
        if (node.children.size() >= 3) {
            BblValue endVal = eval(node.children[2], scope);
            if (endVal.type != BBL::Type::Int) throw BBL::Error{"string.slice: end must be int"};
            end = endVal.intVal;
        }
        // Clamp
        int64_t len = static_cast<int64_t>(data.size());
        if (start < 0) start = 0;
        if (start > len) start = len;
        if (end < 0) end = 0;
        if (end > len) end = len;
        if (start >= end) return BblValue::makeString(intern(""));
        return BblValue::makeString(intern(data.substr(static_cast<size_t>(start), static_cast<size_t>(end - start))));
    }

    // find
    if (method == "find") {
        if (node.children.size() < 2) throw BBL::Error{"string.find requires at least 1 argument"};
        BblValue needleVal = eval(node.children[1], scope);
        if (needleVal.type != BBL::Type::String) throw BBL::Error{"string.find: needle must be string"};
        int64_t startPos = 0;
        if (node.children.size() >= 3) {
            BblValue sv = eval(node.children[2], scope);
            if (sv.type != BBL::Type::Int) throw BBL::Error{"string.find: start must be int"};
            startPos = sv.intVal;
            if (startPos < 0) throw BBL::Error{"string.find: start must be >= 0"};
        }
        size_t pos = data.find(needleVal.stringVal->data, static_cast<size_t>(startPos));
        return BblValue::makeInt(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
    }

    // contains
    if (method == "contains") {
        if (node.children.size() < 2) throw BBL::Error{"string.contains requires 1 argument"};
        BblValue sub = eval(node.children[1], scope);
        if (sub.type != BBL::Type::String) throw BBL::Error{"string.contains: argument must be string"};
        return BblValue::makeBool(data.find(sub.stringVal->data) != std::string::npos);
    }

    // starts-with
    if (method == "starts-with") {
        if (node.children.size() < 2) throw BBL::Error{"string.starts-with requires 1 argument"};
        BblValue prefix = eval(node.children[1], scope);
        if (prefix.type != BBL::Type::String) throw BBL::Error{"string.starts-with: argument must be string"};
        const std::string& p = prefix.stringVal->data;
        return BblValue::makeBool(data.size() >= p.size() && data.compare(0, p.size(), p) == 0);
    }

    // ends-with
    if (method == "ends-with") {
        if (node.children.size() < 2) throw BBL::Error{"string.ends-with requires 1 argument"};
        BblValue suffix = eval(node.children[1], scope);
        if (suffix.type != BBL::Type::String) throw BBL::Error{"string.ends-with: argument must be string"};
        const std::string& s = suffix.stringVal->data;
        return BblValue::makeBool(data.size() >= s.size() && data.compare(data.size() - s.size(), s.size(), s) == 0);
    }

    // replace
    if (method == "replace") {
        if (node.children.size() < 3) throw BBL::Error{"string.replace requires 2 arguments"};
        BblValue oldVal = eval(node.children[1], scope);
        BblValue newVal = eval(node.children[2], scope);
        if (oldVal.type != BBL::Type::String) throw BBL::Error{"string.replace: first argument must be string"};
        if (newVal.type != BBL::Type::String) throw BBL::Error{"string.replace: second argument must be string"};
        const std::string& oldStr = oldVal.stringVal->data;
        const std::string& newStr = newVal.stringVal->data;
        if (oldStr.empty()) throw BBL::Error{"string.replace: search string must not be empty"};
        std::string result = data;
        size_t pos = 0;
        while ((pos = result.find(oldStr, pos)) != std::string::npos) {
            result.replace(pos, oldStr.size(), newStr);
            pos += newStr.size();
        }
        return BblValue::makeString(intern(result));
    }

    // split
    if (method == "split") {
        if (node.children.size() < 2) throw BBL::Error{"string.split requires 1 argument"};
        BblValue sepVal = eval(node.children[1], scope);
        if (sepVal.type != BBL::Type::String) throw BBL::Error{"string.split: separator must be string"};
        const std::string& sep = sepVal.stringVal->data;
        if (sep.empty()) throw BBL::Error{"string.split: separator must not be empty"};
        BblTable* tbl = allocTable();
        size_t start = 0;
        int64_t key = 0;
        while (true) {
            size_t pos = data.find(sep, start);
            if (pos == std::string::npos) {
                tbl->set(BblValue::makeInt(key), BblValue::makeString(intern(data.substr(start))));
                key++;
                break;
            }
            tbl->set(BblValue::makeInt(key), BblValue::makeString(intern(data.substr(start, pos - start))));
            key++;
            start = pos + sep.size();
        }
        tbl->nextIntKey = key;
        return BblValue::makeTable(tbl);
    }

    // join
    if (method == "join") {
        if (node.children.size() < 2) throw BBL::Error{"string.join requires 1 argument"};
        BblValue container = eval(node.children[1], scope);
        std::string result;
        if (container.type == BBL::Type::Table) {
            BblTable* tbl = container.tableVal;
            for (int64_t i = 0; i < tbl->nextIntKey; i++) {
                if (i > 0) result += data;
                BblValue elem = tbl->get(BblValue::makeInt(i)).value_or(BblValue::makeNull());
                result += valueToString(elem);
            }
        } else if (container.type == BBL::Type::Vector) {
            BblVec* vec = container.vectorVal;
            for (size_t i = 0; i < vec->length(); i++) {
                if (i > 0) result += data;
                result += valueToString(readVecElem(vec, i));
            }
        } else {
            throw BBL::Error{"string.join: argument must be table or vector"};
        }
        return BblValue::makeString(intern(result));
    }

    // pad-left
    if (method == "pad-left") {
        if (node.children.size() < 2) throw BBL::Error{"string.pad-left requires at least 1 argument"};
        BblValue widthVal = eval(node.children[1], scope);
        if (widthVal.type != BBL::Type::Int) throw BBL::Error{"string.pad-left: width must be int"};
        int64_t width = widthVal.intVal;
        char fill = ' ';
        if (node.children.size() >= 3) {
            BblValue fillVal = eval(node.children[2], scope);
            if (fillVal.type != BBL::Type::String || fillVal.stringVal->data.size() != 1) {
                throw BBL::Error{"string.pad-left: fill must be a single-character string"};
            }
            fill = fillVal.stringVal->data[0];
        }
        if (static_cast<int64_t>(data.size()) >= width) {
            return BblValue::makeString(obj.stringVal);
        }
        std::string result(static_cast<size_t>(width) - data.size(), fill);
        result += data;
        return BblValue::makeString(intern(result));
    }

    // pad-right
    if (method == "pad-right") {
        if (node.children.size() < 2) throw BBL::Error{"string.pad-right requires at least 1 argument"};
        BblValue widthVal = eval(node.children[1], scope);
        if (widthVal.type != BBL::Type::Int) throw BBL::Error{"string.pad-right: width must be int"};
        int64_t width = widthVal.intVal;
        char fill = ' ';
        if (node.children.size() >= 3) {
            BblValue fillVal = eval(node.children[2], scope);
            if (fillVal.type != BBL::Type::String || fillVal.stringVal->data.size() != 1) {
                throw BBL::Error{"string.pad-right: fill must be a single-character string"};
            }
            fill = fillVal.stringVal->data[0];
        }
        if (static_cast<int64_t>(data.size()) >= width) {
            return BblValue::makeString(obj.stringVal);
        }
        std::string result = data;
        result.append(static_cast<size_t>(width) - data.size(), fill);
        return BblValue::makeString(intern(result));
    }

    throw BBL::Error{"string has no method " + method};
}

BblValue BblState::evalTableMethod(BblTable* tbl, const std::string& method,
                                   const AstNode& node, BblScope& scope) {
    if (method == "length") {
        return BblValue::makeInt(static_cast<int64_t>(tbl->length()));
    }
    if (method == "get") {
        if (node.children.size() < 2) {
            throw BBL::Error{"table.get requires a key argument"};
        }
        BblValue key = eval(node.children[1], scope);
        return tbl->get(key).value_or(BblValue::makeNull());
    }
    if (method == "set") {
        if (node.children.size() < 3) {
            throw BBL::Error{"table.set requires key and value arguments"};
        }
        BblValue key = eval(node.children[1], scope);
        BblValue val = eval(node.children[2], scope);
        tbl->set(key, val);
        return BblValue::makeNull();
    }
    if (method == "delete") {
        if (node.children.size() < 2) {
            throw BBL::Error{"table.delete requires a key argument"};
        }
        BblValue key = eval(node.children[1], scope);
        tbl->del(key);
        return BblValue::makeNull();
    }
    if (method == "has") {
        if (node.children.size() < 2) {
            throw BBL::Error{"table.has requires a key argument"};
        }
        BblValue key = eval(node.children[1], scope);
        return BblValue::makeBool(tbl->has(key));
    }
    if (method == "keys") {
        BblTable* result = allocTable();
        int64_t idx = 0;
        for (auto& k : tbl->order) {
            result->set(BblValue::makeInt(idx), k);
            idx++;
        }
        return BblValue::makeTable(result);
    }
    if (method == "push") {
        if (node.children.size() < 2) {
            throw BBL::Error{"table.push requires a value argument"};
        }
        BblValue val = eval(node.children[1], scope);
        BblValue key = BblValue::makeInt(tbl->nextIntKey);
        tbl->set(key, val);
        return BblValue::makeNull();
    }
    if (method == "pop") {
        // Pop last integer key
        for (auto it = tbl->order.rbegin(); it != tbl->order.rend(); ++it) {
            if (it->type == BBL::Type::Int) {
                BblValue val = tbl->get(*it).value_or(BblValue::makeNull());
                tbl->del(*it);
                return val;
            }
        }
        throw BBL::Error{"table.pop: no integer keys"};
    }
    if (method == "at") {
        if (node.children.size() < 2) {
            throw BBL::Error{"table.at requires an index argument"};
        }
        BblValue idx = eval(node.children[1], scope);
        if (idx.type != BBL::Type::Int) {
            throw BBL::Error{"table.at: index must be int"};
        }
        int64_t pos = idx.intVal;
        if (pos < 0 || static_cast<size_t>(pos) >= tbl->order.size())
            throw BBL::Error{"table.at: index " + std::to_string(pos) + " out of bounds"};
        BblValue key = tbl->order[static_cast<size_t>(pos)];
        return tbl->get(key).value_or(BblValue::makeNull());
    }
    throw BBL::Error{"table has no method " + method};
}

BblValue BblState::callFn(BblFn* fn, const BblValue* args, size_t argc, int callLine) {
    if (activeScopes.size() >= maxCallDepth) {
        throw BBL::Error{"stack overflow: exceeded " + std::to_string(maxCallDepth) + " frames"};
    }
    if (argc != fn->params.size()) {
        throw BBL::Error{"arity mismatch: expected " + std::to_string(fn->params.size())
                         + " argument(s), got " + std::to_string(argc)};
    }

    // Fresh scope for the call — flat mode with slots for captures + params
    BblScope callScope;
    callScope.slotMap = &fn->slotIndex;
    callScope.slots.resize(fn->slotIndex.size());
    activeScopes.push_back(&callScope);
    // Load captures into slots
    for (auto& [id, val] : fn->captures) {
        callScope.slots[fn->slotIndex.at(id)] = val;
    }
    // Bind args into slots by direct offset (no hash lookup)
    for (size_t i = 0; i < argc; i++) {
        callScope.slots[fn->paramSlotStart + i] = args[i];
    }

    BblFn* prevFn = currentFn;
    currentFn = fn;
    BblValue result = BblValue::makeNull();
    bool done = false;
    while (!done) {
        try {
            for (auto& node : fn->body) {
                result = eval(node, callScope);
                checkTerminated();
                checkStepLimit();
                if (flowSignal) break;
            }
            done = true;
        } catch (TailCall& tc) {
            try {
                checkTerminated();
                checkStepLimit();
            } catch (...) {
                currentFn = prevFn;
                activeScopes.pop_back();
                throw;
            }
            // Rebind params in-place
            for (size_t i = 0; i < tc.argc; i++) {
                callScope.slots[fn->paramSlotStart + i] =
                    (tc.argc <= 8) ? tc.args[i] : tc.heapArgs[i];
            }
            // Reload captures (body may have mutated shadowing locals)
            for (auto& [id, val] : fn->captures) {
                callScope.slots[fn->slotIndex.at(id)] = val;
            }
            // Continue loop — no new stack frame
        } catch (...) {
            currentFn = prevFn;
            activeScopes.pop_back();
            throw;
        }
    }
    currentFn = prevFn;
    activeScopes.pop_back();
    if (flowSignal) {
        const char* msg = (flowSignal == FlowBreak) ? "break outside of loop" : "continue outside of loop";
        flowSignal = FlowNone;
        throw BBL::Error{msg};
    }
    return result;
}

// ---------- exec / execfile ----------

void BblState::exec(const std::string& source) {
    BblLexer lexer(source.c_str());
    auto nodes = parse(lexer);
    if (useBytecode) {
        Chunk chunk = compile(*this, nodes);
        if (useJit) {
            auto result = jitExecute(*this, chunk);
            (void)result;
            return;
        }
        auto result = vmExecute(*this, chunk);
        if (result != INTERPRET_OK)
            throw BBL::Error{"bytecode execution failed"};
        return;
    }
    for (auto& node : nodes) {
        if (allocCount >= gcThreshold) gc();
        eval(node, rootScope);
        if (flowSignal) {
            const char* msg = (flowSignal == FlowBreak) ? "break outside of loop" : "continue outside of loop";
            flowSignal = FlowNone;
            throw BBL::Error{msg};
        }
    }
}

BblValue BblState::execExpr(const std::string& source) {
    BblLexer lexer(source.c_str());
    auto nodes = parse(lexer);
    if (useBytecode) {
        Chunk chunk = compile(*this, nodes);
        if (useJit) {
            return jitExecute(*this, chunk);
        }
        auto result = vmExecute(*this, chunk);
        if (result != INTERPRET_OK)
            throw BBL::Error{"bytecode execution failed"};
        return vm->stack[0];
    }
    BblValue result;
    result.type = BBL::Type::Null;
    for (auto& node : nodes) {
        if (allocCount >= gcThreshold) gc();
        result = eval(node, rootScope);
        if (flowSignal) {
            const char* msg = (flowSignal == FlowBreak) ? "break outside of loop" : "continue outside of loop";
            flowSignal = FlowNone;
            throw BBL::Error{msg};
        }
    }
    return result;
}

std::filesystem::path BblState::resolveSandboxPath(const std::string& path, const char* context) {
    namespace fs = std::filesystem;
    fs::path fspath(path);
    if (!allowOpenFilesystem) {
        if (fspath.is_absolute()) {
            throw BBL::Error{std::string(context) + ": absolute paths not allowed: " + path};
        }
        // Resolve relative to scriptDir, then canonicalize to catch traversal
        fs::path base = scriptDir.empty() ? fs::current_path() : fs::path(scriptDir);
        fs::path joined = base / fspath;
        fs::path canonical = fs::weakly_canonical(joined);
        fs::path canonBase = fs::weakly_canonical(base);
        // The canonical path must start with the canonical base
        auto rel = canonical.lexically_relative(canonBase);
        if (rel.empty() || *rel.begin() == "..") {
            throw BBL::Error{std::string(context) + ": path escapes sandbox: " + path};
        }
        return canonical;
    }
    // Open filesystem: just resolve relative to scriptDir
    fs::path resolved;
    if (fspath.is_absolute()) {
        resolved = fspath;
    } else if (scriptDir.empty()) {
        resolved = fspath;
    } else {
        resolved = fs::path(scriptDir) / fspath;
    }
    return resolved;
}

void BblState::execfile(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path resolved = resolveSandboxPath(path, "execfile");
    if (!fs::exists(resolved)) {
        // Try BBL_PATH directories
        const char* bblPath = std::getenv("BBL_PATH");
        if (bblPath) {
            std::string pathStr(bblPath);
            size_t pos = 0;
            while (pos < pathStr.size()) {
                size_t sep = pathStr.find(':', pos);
                if (sep == std::string::npos) sep = pathStr.size();
                std::string dir = pathStr.substr(pos, sep - pos);
                if (!dir.empty()) {
                    fs::path candidate = fs::path(dir) / path;
                    if (fs::exists(candidate)) {
                        resolved = candidate;
                        break;
                    }
                }
                pos = sep + 1;
            }
        }
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
    uint32_t id = resolveSymbol(name);
    return rootScope.bindings->count(id) > 0;
}

std::expected<BBL::Type, BBL::GetError> BblState::getType(const std::string& name) const {
    uint32_t id = resolveSymbol(name);
    auto it = rootScope.bindings->find(id);
    if (it == rootScope.bindings->end()) {
        return std::unexpected(BBL::GetError::NotFound);
    }
    return it->second.type;
}

std::expected<BblValue, BBL::GetError> BblState::get(const std::string& name) const {
    uint32_t id = resolveSymbol(name);
    auto it = rootScope.bindings->find(id);
    if (it == rootScope.bindings->end()) {
        return std::unexpected(BBL::GetError::NotFound);
    }
    return it->second;
}

std::expected<int64_t, BBL::GetError> BblState::getInt(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::Int) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->intVal;
}

std::expected<double, BBL::GetError> BblState::getFloat(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::Float) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->floatVal;
}

std::expected<bool, BBL::GetError> BblState::getBool(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::Bool) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->boolVal;
}

std::expected<const char*, BBL::GetError> BblState::getString(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::String) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->stringVal->data.c_str();
}

std::expected<BblTable*, BBL::GetError> BblState::getTable(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::Table) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->tableVal;
}

std::expected<BblBinary*, BBL::GetError> BblState::getBinary(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type != BBL::Type::Binary) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->binaryVal;
}

// ---------- Setters ----------

void BblState::setInt(const std::string& name, int64_t val) {
    rootScope.def(resolveSymbol(name), BblValue::makeInt(val));
}

void BblState::setFloat(const std::string& name, double val) {
    rootScope.def(resolveSymbol(name), BblValue::makeFloat(val));
}

void BblState::setString(const std::string& name, const char* str) {
    rootScope.def(resolveSymbol(name), BblValue::makeString(intern(str)));
}

void BblState::set(const std::string& name, BblValue val) {
    rootScope.def(resolveSymbol(name), val);
}

void BblState::setBinary(const std::string& name, const uint8_t* ptr, size_t size) {
    std::vector<uint8_t> data(ptr, ptr + size);
    rootScope.def(resolveSymbol(name), BblValue::makeBinary(allocBinary(std::move(data))));
}

void BblState::pushUserData(const std::string& typeName, void* ptr) {
    returnValue = BblValue::makeUserData(allocUserData(typeName, ptr));
    hasReturn = true;
}

// ---------- C function registration ----------

void BblState::defn(const std::string& name, BblCFunction fn) {
    rootScope.def(resolveSymbol(name), BblValue::makeCFn(fn));
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

void BblState::pushTable(BblTable* tbl) {
    returnValue = BblValue::makeTable(tbl);
    hasReturn = true;
}

void BblState::pushBinary(const uint8_t* ptr, size_t size) {
    std::vector<uint8_t> data(ptr, ptr + size);
    returnValue = BblValue::makeBinary(allocBinary(std::move(data)));
    hasReturn = true;
}

// ---------- StructBuilder ----------

BBL::StructBuilder::StructBuilder(const std::string& name, size_t totalSize)
    : name_(name), totalSize_(totalSize) {}

void BBL::StructBuilder::addField(const std::string& fname, size_t offset, size_t fsize, CType ct, const std::string& stName) {
    if (offset + fsize > totalSize_) {
        throw BBL::Error{"struct " + name_ + ": field " + fname + " exceeds total size"};
    }
    for (auto& f : fields_) {
        size_t aStart = f.offset, aEnd = f.offset + f.size;
        size_t bStart = offset, bEnd = offset + fsize;
        if (aStart < bEnd && bStart < aEnd) {
            throw BBL::Error{"struct " + name_ + ": field " + fname + " overlaps with " + f.name};
        }
    }
    fields_.push_back(FieldDesc{fname, offset, fsize, ct, stName});
}

BBL::StructBuilder& BBL::StructBuilder::structField(const std::string& fname, size_t offset, const std::string& typeName) {
    // size will be resolved at registration time
    addField(fname, offset, 0, CType::Struct, typeName);
    return *this;
}

void BblState::registerStruct(const BBL::StructBuilder& builder) {
    auto it = structDescs.find(builder.name());
    if (it != structDescs.end()) {
        return; // silent no-op on re-registration
    }
    StructDesc desc;
    desc.name = builder.name();
    desc.totalSize = builder.totalSize();
    for (auto& f : builder.fields()) {
        FieldDesc fd = f;
        if (fd.ctype == CType::Struct) {
            auto sit = structDescs.find(fd.structType);
            if (sit == structDescs.end()) {
                throw BBL::Error{"struct " + desc.name + ": field " + fd.name + " references unknown struct " + fd.structType};
            }
            fd.size = sit->second.totalSize;
        }
        desc.fields.push_back(fd);
    }
    structDescs[desc.name] = std::move(desc);
}

void BblState::registerType(const BBL::TypeBuilder& builder) {
    auto it = userDataDescs.find(builder.name());
    if (it != userDataDescs.end()) {
        return;
    }
    UserDataDesc desc;
    desc.name = builder.name();
    desc.methods = builder.methods();
    desc.destructor = builder.getDestructor();
    userDataDescs[desc.name] = std::move(desc);
}

// ---------- Struct field read/write ----------

BblValue BblState::readField(BblStruct* s, const FieldDesc& fd) {
    const uint8_t* p = s->data.data() + fd.offset;
    switch (fd.ctype) {
        case CType::Int8: {
            int8_t v;
            memcpy(&v, p, 1);
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Uint8: {
            uint8_t v;
            memcpy(&v, p, 1);
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Int16: {
            int16_t v;
            memcpy(&v, p, 2);
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Uint16: {
            uint16_t v;
            memcpy(&v, p, 2);
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Float32: {
            float v;
            memcpy(&v, p, sizeof(float));
            return BblValue::makeFloat(static_cast<double>(v));
        }
        case CType::Float64: {
            double v;
            memcpy(&v, p, sizeof(double));
            return BblValue::makeFloat(v);
        }
        case CType::Int32: {
            int32_t v;
            memcpy(&v, p, sizeof(int32_t));
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Uint32: {
            uint32_t v;
            memcpy(&v, p, sizeof(uint32_t));
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Int64: {
            int64_t v;
            memcpy(&v, p, sizeof(int64_t));
            return BblValue::makeInt(v);
        }
        case CType::Uint64: {
            uint64_t v;
            memcpy(&v, p, sizeof(uint64_t));
            return BblValue::makeInt(static_cast<int64_t>(v));
        }
        case CType::Bool: {
            return BblValue::makeBool(*p != 0);
        }
        case CType::Struct: {
            auto sit = structDescs.find(fd.structType);
            if (sit == structDescs.end()) {
                throw BBL::Error{"unknown struct type: " + fd.structType};
            }
            BblStruct* inner = allocStruct(&sit->second);
            memcpy(inner->data.data(), p, fd.size);
            return BblValue::makeStruct(inner);
        }
    }
    throw BBL::Error{"internal: unknown CType"};
}

void BblState::writeField(BblStruct* s, const FieldDesc& fd, const BblValue& val) {
    uint8_t* p = s->data.data() + fd.offset;
    switch (fd.ctype) {
        case CType::Int8: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            int8_t v = static_cast<int8_t>(val.intVal);
            memcpy(p, &v, 1);
            return;
        }
        case CType::Uint8: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            uint8_t v = static_cast<uint8_t>(val.intVal);
            memcpy(p, &v, 1);
            return;
        }
        case CType::Int16: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            int16_t v = static_cast<int16_t>(val.intVal);
            memcpy(p, &v, 2);
            return;
        }
        case CType::Uint16: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            uint16_t v = static_cast<uint16_t>(val.intVal);
            memcpy(p, &v, 2);
            return;
        }
        case CType::Float32: {
            if (val.type == BBL::Type::Float) {
                float v = static_cast<float>(val.floatVal);
                memcpy(p, &v, sizeof(float));
            } else if (val.type == BBL::Type::Int) {
                float v = static_cast<float>(val.intVal);
                memcpy(p, &v, sizeof(float));
            } else {
                throw BBL::Error{"type mismatch: expected numeric, got " + typeName(val.type)};
            }
            return;
        }
        case CType::Float64: {
            if (val.type == BBL::Type::Float) {
                memcpy(p, &val.floatVal, sizeof(double));
            } else if (val.type == BBL::Type::Int) {
                double v = static_cast<double>(val.intVal);
                memcpy(p, &v, sizeof(double));
            } else {
                throw BBL::Error{"type mismatch: expected numeric, got " + typeName(val.type)};
            }
            return;
        }
        case CType::Int32: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            int32_t v = static_cast<int32_t>(val.intVal);
            memcpy(p, &v, sizeof(int32_t));
            return;
        }
        case CType::Uint32: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            uint32_t v = static_cast<uint32_t>(val.intVal);
            memcpy(p, &v, sizeof(uint32_t));
            return;
        }
        case CType::Int64: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            memcpy(p, &val.intVal, sizeof(int64_t));
            return;
        }
        case CType::Uint64: {
            if (val.type != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type)};
            }
            uint64_t v = static_cast<uint64_t>(val.intVal);
            memcpy(p, &v, sizeof(uint64_t));
            return;
        }
        case CType::Bool: {
            if (val.type != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: expected bool, got " + typeName(val.type)};
            }
            uint8_t v = val.boolVal ? 1 : 0;
            *p = v;
            return;
        }
        case CType::Struct: {
            if (val.type != BBL::Type::Struct || val.structVal->desc->name != fd.structType) {
                throw BBL::Error{"type mismatch: expected struct " + fd.structType};
            }
            memcpy(p, val.structVal->data.data(), fd.size);
            return;
        }
    }
}

BblValue BblState::constructStruct(StructDesc* desc, const std::vector<BblValue>& args, int callLine) {
    if (args.size() != desc->fields.size()) {
        throw BBL::Error{"struct " + desc->name + ": expected " + std::to_string(desc->fields.size())
                         + " argument(s), got " + std::to_string(args.size())};
    }
    BblStruct* s = allocStruct(desc);
    for (size_t i = 0; i < desc->fields.size(); i++) {
        writeField(s, desc->fields[i], args[i]);
    }
    return BblValue::makeStruct(s);
}

// ---------- Vector helpers ----------

static size_t toIndex(int64_t val, size_t length, const char* context) {
    if (val < 0 || static_cast<size_t>(val) >= length) {
        throw BBL::Error{std::string(context) + ": index " + std::to_string(val)
                         + " out of bounds (length " + std::to_string(length) + ")"};
    }
    return static_cast<size_t>(val);
}

BblValue BblState::readVecElem(BblVec* vec, size_t i) {
    if (i >= vec->length()) {
        throw BBL::Error{"vector index " + std::to_string(i) + " out of bounds (length " + std::to_string(vec->length()) + ")"};
    }
    const uint8_t* p = vec->at(i);
    switch (vec->elemTypeTag) {
        case BBL::Type::Int: {
            int64_t v;
            memcpy(&v, p, sizeof(int64_t));
            return BblValue::makeInt(v);
        }
        case BBL::Type::Float: {
            double v;
            memcpy(&v, p, sizeof(double));
            return BblValue::makeFloat(v);
        }
        case BBL::Type::Bool: {
            return BblValue::makeBool(*p != 0);
        }
        case BBL::Type::Struct: {
            auto sit = structDescs.find(vec->elemType);
            if (sit == structDescs.end()) {
                throw BBL::Error{"unknown struct type: " + vec->elemType};
            }
            BblStruct* s = allocStruct(&sit->second);
            memcpy(s->data.data(), p, vec->elemSize);
            return BblValue::makeStruct(s);
        }
        default:
            throw BBL::Error{"internal: unsupported vector element type"};
    }
}

void BblState::writeVecElem(BblVec* vec, size_t i, const BblValue& val) {
    if (i >= vec->length()) {
        throw BBL::Error{"vector index " + std::to_string(i) + " out of bounds"};
    }
    uint8_t* p = vec->data.data() + i * vec->elemSize;
    switch (vec->elemTypeTag) {
        case BBL::Type::Int:
            if (val.type != BBL::Type::Int)
                throw BBL::Error{"vector type mismatch: expected int, got " + typeName(val.type)};
            memcpy(p, &val.intVal, sizeof(int64_t));
            return;
        case BBL::Type::Float: {
            double v;
            if (val.type == BBL::Type::Float) v = val.floatVal;
            else if (val.type == BBL::Type::Int) v = static_cast<double>(val.intVal);
            else throw BBL::Error{"vector type mismatch: expected float, got " + typeName(val.type)};
            memcpy(p, &v, sizeof(double));
            return;
        }
        case BBL::Type::Bool:
            if (val.type != BBL::Type::Bool)
                throw BBL::Error{"vector type mismatch: expected bool, got " + typeName(val.type)};
            *p = val.boolVal ? 1 : 0;
            return;
        case BBL::Type::Struct:
            if (val.type != BBL::Type::Struct || val.structVal->desc->name != vec->elemType)
                throw BBL::Error{"vector type mismatch: expected struct " + vec->elemType};
            memcpy(p, val.structVal->data.data(), vec->elemSize);
            return;
        default:
            throw BBL::Error{"internal: unsupported vector element type"};
    }
}

void BblState::packValue(BblVec* vec, const BblValue& val) {
    size_t oldSize = vec->data.size();
    vec->data.resize(oldSize + vec->elemSize, 0);
    uint8_t* p = vec->data.data() + oldSize;
    switch (vec->elemTypeTag) {
        case BBL::Type::Int: {
            if (val.type != BBL::Type::Int) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected int, got " + typeName(val.type)};
            }
            memcpy(p, &val.intVal, sizeof(int64_t));
            return;
        }
        case BBL::Type::Float: {
            double v;
            if (val.type == BBL::Type::Float) {
                v = val.floatVal;
            } else if (val.type == BBL::Type::Int) {
                v = static_cast<double>(val.intVal);
            } else {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected float, got " + typeName(val.type)};
            }
            memcpy(p, &v, sizeof(double));
            return;
        }
        case BBL::Type::Bool: {
            if (val.type != BBL::Type::Bool) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected bool, got " + typeName(val.type)};
            }
            *p = val.boolVal ? 1 : 0;
            return;
        }
        case BBL::Type::Struct: {
            if (val.type != BBL::Type::Struct || val.structVal->desc->name != vec->elemType) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected struct " + vec->elemType};
            }
            memcpy(p, val.structVal->data.data(), vec->elemSize);
            return;
        }
        default:
            vec->data.resize(oldSize);
            throw BBL::Error{"internal: unsupported vector element type"};
    }
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

static std::string valueToString(const BblValue& val) {
    char buf[64];
    switch (val.type) {
        case BBL::Type::String:
            return val.stringVal->data;
        case BBL::Type::Int:
            snprintf(buf, sizeof(buf), "%" PRId64, val.intVal);
            return buf;
        case BBL::Type::Float:
            snprintf(buf, sizeof(buf), "%g", val.floatVal);
            return buf;
        case BBL::Type::Bool:
            return val.boolVal ? "true" : "false";
        case BBL::Type::Null:
            return "null";
        case BBL::Type::Binary:
            snprintf(buf, sizeof(buf), "<binary %zu bytes>", val.binaryVal->length());
            return buf;
        case BBL::Type::Fn:
            return val.isCFn ? "<cfn>" : "<fn>";
        case BBL::Type::Table:
            return "<table length=" + std::to_string(val.tableVal->length()) + ">";
        case BBL::Type::Vector:
            return "<vector " + val.vectorVal->elemType + " length=" + std::to_string(val.vectorVal->length()) + ">";
        case BBL::Type::Struct:
            return "<struct " + val.structVal->desc->name + ">";
        case BBL::Type::UserData:
            return "<userdata " + val.userdataVal->desc->name + ">";
        default:
            return "<unknown>";
    }
}

static int bblPrint(BblState* bbl) {
    int count = bbl->argCount();
    for (int i = 0; i < count; i++) {
        std::string s = valueToString(bbl->getArg(i));
        if (bbl->printCapture) {
            *bbl->printCapture += s;
        } else {
            fputs(s.c_str(), stdout);
        }
    }
    return 0;
}

static int bblStr(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"str requires 1 argument"};
    std::string s = valueToString(bbl->getArg(0));
    bbl->pushString(s.c_str());
    return 1;
}

static int bblTypeof(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"typeof requires 1 argument"};
    bbl->pushString(typeName(bbl->getArg(0).type).c_str());
    return 1;
}

static int bblInt(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"int requires 1 argument"};
    BblValue arg = bbl->getArg(0);
    switch (arg.type) {
        case BBL::Type::Int:
            bbl->pushInt(arg.intVal);
            return 1;
        case BBL::Type::Float:
            bbl->pushInt(static_cast<int64_t>(arg.floatVal));
            return 1;
        case BBL::Type::String: {
            const char* str = arg.stringVal->data.c_str();
            char* end = nullptr;
            errno = 0;
            int64_t val = strtoll(str, &end, 10);
            if (end == str || *end != '\0') {
                throw BBL::Error{"int: cannot parse \"" + arg.stringVal->data + "\""};
            }
            if (errno == ERANGE) {
                throw BBL::Error{"int: overflow parsing \"" + arg.stringVal->data + "\""};
            }
            bbl->pushInt(val);
            return 1;
        }
        default:
            throw BBL::Error{"int: cannot convert " + typeName(arg.type) + " to int"};
    }
}

static int bblFloat(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"float requires 1 argument"};
    BblValue arg = bbl->getArg(0);
    switch (arg.type) {
        case BBL::Type::Float:
            bbl->pushFloat(arg.floatVal);
            return 1;
        case BBL::Type::Int:
            bbl->pushFloat(static_cast<double>(arg.intVal));
            return 1;
        case BBL::Type::String: {
            const char* str = arg.stringVal->data.c_str();
            char* end = nullptr;
            errno = 0;
            double val = strtod(str, &end);
            if (end == str || *end != '\0') {
                throw BBL::Error{"float: cannot parse \"" + arg.stringVal->data + "\""};
            }
            if (errno == ERANGE) {
                throw BBL::Error{"float: overflow parsing \"" + arg.stringVal->data + "\""};
            }
            bbl->pushFloat(val);
            return 1;
        }
        default:
            throw BBL::Error{"float: cannot convert " + typeName(arg.type) + " to float"};
    }
}

static int bblFmt(BblState* bbl) {
    if (bbl->argCount() < 1) throw BBL::Error{"fmt requires at least 1 argument (format string)"};
    BblValue fmtArg = bbl->getArg(0);
    if (fmtArg.type != BBL::Type::String) throw BBL::Error{"fmt: first argument must be a string"};
    const std::string& fmt = fmtArg.stringVal->data;

    std::string result;
    result.reserve(fmt.size() * 2);
    int argIdx = 1;
    int argCount = bbl->argCount();

    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                result += '{';
                i++;
            } else if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                if (argIdx >= argCount) {
                    throw BBL::Error{"fmt: not enough arguments for format string"};
                }
                result += valueToString(bbl->getArg(argIdx));
                argIdx++;
                i++;
            } else {
                throw BBL::Error{"fmt: lone '{' in format string"};
            }
        } else if (c == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                result += '}';
                i++;
            } else {
                throw BBL::Error{"fmt: lone '}' in format string"};
            }
        } else {
            result += c;
        }
    }

    if (argIdx != argCount) {
        throw BBL::Error{"fmt: too many arguments for format string"};
    }

    bbl->pushString(result.c_str());
    return 1;
}

void BBL::addPrint(BblState& bbl) {
    bbl.defn("print", bblPrint);
    bbl.defn("str", bblStr);
    bbl.defn("typeof", bblTypeof);
    bbl.defn("int", bblInt);
    bbl.defn("float", bblFloat);
    bbl.defn("fmt", bblFmt);
}

// ---------- addFileIo ----------

static int bblFilebytes(BblState* bbl) {
    if (bbl->argCount() < 1) {
        throw BBL::Error{"filebytes requires a path argument"};
    }
    const char* path = bbl->getStringArg(0);
    namespace fs = std::filesystem;
    fs::path resolved = bbl->resolveSandboxPath(path, "filebytes");
    std::ifstream file(resolved, std::ios::binary);
    if (!file.is_open()) {
        throw BBL::Error{"filebytes: file not found: " + resolved.string()};
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    BblBinary* b = bbl->allocBinary(std::move(data));
    bbl->returnValue = BblValue::makeBinary(b);
    bbl->hasReturn = true;
    return 0;
}

static int bblFopen(BblState* bbl) {
    if (bbl->argCount() < 1) {
        throw BBL::Error{"fopen requires a path argument"};
    }
    const char* path = bbl->getStringArg(0);
    const char* mode = "r";
    if (bbl->argCount() >= 2) {
        mode = bbl->getStringArg(1);
    }
    auto resolved = bbl->resolveSandboxPath(path, "fopen");
    FILE* fp = fopen(resolved.c_str(), mode);
    if (!fp) {
        throw BBL::Error{"fopen: cannot open file: " + std::string(path)};
    }
    bbl->pushUserData("File", fp);
    return 0;
}

static int bblFileRead(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData || self.userdataVal->desc->name != "File") {
        throw BBL::Error{"File.read: expected File object"};
    }
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (!fp) throw BBL::Error{"File.read: file is closed"};
    std::string contents;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), fp)) {
        contents.append(buf, n);
    }
    bbl->pushString(contents.c_str());
    return 0;
}

static int bblFileReadBytes(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.read-bytes: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (!fp) throw BBL::Error{"File.read-bytes: file is closed"};
    int64_t n = bbl->getIntArg(1);
    std::vector<uint8_t> data(static_cast<size_t>(n));
    size_t got = fread(data.data(), 1, static_cast<size_t>(n), fp);
    data.resize(got);
    bbl->pushBinary(data.data(), data.size());
    return 0;
}

static int bblFileWrite(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.write: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (!fp) throw BBL::Error{"File.write: file is closed"};
    const char* str = bbl->getStringArg(1);
    size_t len = strlen(str);
    size_t written = fwrite(str, 1, len, fp);
    if (written != len) throw BBL::Error{"File.write: write failed"};
    return 0;
}

static int bblFileWriteBytes(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.write-bytes: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (!fp) throw BBL::Error{"File.write-bytes: file is closed"};
    BblBinary* b = bbl->getBinaryArg(1);
    size_t written = fwrite(b->data.data(), 1, b->length(), fp);
    if (written != b->length()) throw BBL::Error{"File.write-bytes: write failed"};
    return 0;
}

static int bblFileReadLine(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.read-line: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (!fp) throw BBL::Error{"File.read-line: file is closed"};
    std::string result;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        bbl->pushNull();
        return 0;
    }
    result = buf;
    while (!result.empty() && result.back() != '\n' && !feof(fp)) {
        if (!fgets(buf, sizeof(buf), fp)) break;
        result += buf;
    }
    if (!result.empty() && result.back() == '\n') result.pop_back();
    if (!result.empty() && result.back() == '\r') result.pop_back();
    bbl->pushString(result.c_str());
    return 0;
}

static int bblFileClose(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.close: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (fp && fp != stdin && fp != stdout && fp != stderr) {
        fclose(fp);
        self.userdataVal->data = nullptr;
    }
    return 0;
}

static int bblFileFlush(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData) throw BBL::Error{"File.flush: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal->data);
    if (fp) fflush(fp);
    return 0;
}

static void fileDestructor(void* ptr) {
    FILE* fp = static_cast<FILE*>(ptr);
    if (fp && fp != stdin && fp != stdout && fp != stderr) fclose(fp);
}

void BBL::addFileIo(BblState& bbl) {
    if (bbl.has("filebytes")) return;
    BBL::TypeBuilder fb("File");
    fb.method("read", bblFileRead)
      .method("read-bytes", bblFileReadBytes)
      .method("read-line", bblFileReadLine)
      .method("write", bblFileWrite)
      .method("write-bytes", bblFileWriteBytes)
      .method("close", bblFileClose)
      .method("flush", bblFileFlush)
      .destructor(fileDestructor);
    bbl.registerType(fb);
    bbl.defn("filebytes", bblFilebytes);
    bbl.defn("fopen", bblFopen);
    bbl.set("stdin", BblValue::makeUserData(bbl.allocUserData("File", static_cast<void*>(stdin))));
    bbl.set("stdout", BblValue::makeUserData(bbl.allocUserData("File", static_cast<void*>(stdout))));
    bbl.set("stderr", BblValue::makeUserData(bbl.allocUserData("File", static_cast<void*>(stderr))));
}

// ---------- addMath ----------

static double getNumericArg(BblState* bbl, int i) {
    BBL::Type t = bbl->getArgType(i);
    if (t == BBL::Type::Float) return bbl->getFloatArg(i);
    if (t == BBL::Type::Int) return static_cast<double>(bbl->getIntArg(i));
    throw BBL::Error{"math: expected numeric argument, got " + typeName(t)};
}

#define MATH_UNARY(NAME, FN) \
    static int bblMath_##NAME(BblState* bbl) { \
        double x = getNumericArg(bbl, 0); \
        bbl->pushFloat(FN(x)); \
        return 0; \
    }

#define MATH_BINARY(NAME, FN) \
    static int bblMath_##NAME(BblState* bbl) { \
        double a = getNumericArg(bbl, 0); \
        double b = getNumericArg(bbl, 1); \
        bbl->pushFloat(FN(a, b)); \
        return 0; \
    }

MATH_UNARY(sin, std::sin)
MATH_UNARY(cos, std::cos)
MATH_UNARY(tan, std::tan)
MATH_UNARY(asin, std::asin)
MATH_UNARY(acos, std::acos)
MATH_UNARY(atan, std::atan)
MATH_UNARY(floor, std::floor)
MATH_UNARY(ceil, std::ceil)
MATH_UNARY(exp, std::exp)
MATH_UNARY(log, std::log)
MATH_UNARY(log2, std::log2)
MATH_UNARY(log10, std::log10)
MATH_BINARY(atan2, std::atan2)
MATH_BINARY(pow, std::pow)
MATH_BINARY(fmin, std::fmin)
MATH_BINARY(fmax, std::fmax)

static int bblMath_sqrt(BblState* bbl) {
    double x = getNumericArg(bbl, 0);
    if (x < 0) throw BBL::Error{"sqrt: negative argument"};
    bbl->pushFloat(std::sqrt(x));
    return 0;
}

static int bblMath_abs(BblState* bbl) {
    double x = getNumericArg(bbl, 0);
    bbl->pushFloat(std::fabs(x));
    return 0;
}

void BBL::addMath(BblState& bbl) {
    if (bbl.has("sin")) return;
    bbl.defn("sin", bblMath_sin);
    bbl.defn("cos", bblMath_cos);
    bbl.defn("tan", bblMath_tan);
    bbl.defn("asin", bblMath_asin);
    bbl.defn("acos", bblMath_acos);
    bbl.defn("atan", bblMath_atan);
    bbl.defn("atan2", bblMath_atan2);
    bbl.defn("sqrt", bblMath_sqrt);
    bbl.defn("abs", bblMath_abs);
    bbl.defn("floor", bblMath_floor);
    bbl.defn("ceil", bblMath_ceil);
    bbl.defn("min", bblMath_fmin);
    bbl.defn("max", bblMath_fmax);
    bbl.defn("pow", bblMath_pow);
    bbl.defn("log", bblMath_log);
    bbl.defn("log2", bblMath_log2);
    bbl.defn("log10", bblMath_log10);
    bbl.defn("exp", bblMath_exp);
    bbl.setFloat("pi", 3.14159265358979323846);
    bbl.setFloat("e", 2.71828182845904523536);
}

// ---------- addOs ----------

struct BblProcess { FILE* pipe; bool waited; };

static void processDestructor(void* data) {
    auto* proc = static_cast<BblProcess*>(data);
    if (!proc->waited && proc->pipe) pclose(proc->pipe);
    delete proc;
}

static int bblOs_getenv(BblState* bbl) {
    const char* name = bbl->getStringArg(0);
    const char* val = getenv(name);
    if (val) bbl->pushString(val); else bbl->pushNull();
    return 0;
}

static int bblOs_setenv(BblState* bbl) {
    setenv(bbl->getStringArg(0), bbl->getStringArg(1), 1);
    bbl->pushNull();
    return 0;
}

static int bblOs_unsetenv(BblState* bbl) {
    unsetenv(bbl->getStringArg(0));
    bbl->pushNull();
    return 0;
}

static int bblOs_clock(BblState* bbl) {
    bbl->pushFloat((double)clock() / CLOCKS_PER_SEC);
    return 0;
}

static int bblOs_time(BblState* bbl) {
    bbl->pushInt((int64_t)::time(nullptr));
    return 0;
}

static int bblOs_sleep(BblState* bbl) {
    double secs = bbl->getFloatArg(0);
    if (secs < 0) secs = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
    bbl->pushNull();
    return 0;
}

static int bblOs_exit(BblState* bbl) {
    int code = bbl->argCount() > 0 ? (int)bbl->getIntArg(0) : 0;
    std::exit(code);
    return 0;
}

static int bblOs_getpid(BblState* bbl) {
    bbl->pushInt((int64_t)getpid());
    return 0;
}

static int bblOs_getcwd(BblState* bbl) {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) bbl->pushString(buf);
    else bbl->pushNull();
    return 0;
}

static int bblOs_chdir(BblState* bbl) {
    bbl->pushBool(chdir(bbl->getStringArg(0)) == 0);
    return 0;
}

static int bblOs_mkdir(BblState* bbl) {
    bbl->pushBool(::mkdir(bbl->getStringArg(0), 0755) == 0);
    return 0;
}

static int bblOs_remove(BblState* bbl) {
    bbl->pushBool(::remove(bbl->getStringArg(0)) == 0);
    return 0;
}

static int bblOs_rename(BblState* bbl) {
    bbl->pushBool(::rename(bbl->getStringArg(0), bbl->getStringArg(1)) == 0);
    return 0;
}

static int bblOs_tmpname(BblState* bbl) {
    char tmpl[] = "/tmp/bbl_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) { close(fd); bbl->pushString(tmpl); }
    else bbl->pushNull();
    return 0;
}

static int bblOs_execute(BblState* bbl) {
    int raw = system(bbl->getStringArg(0));
    if (raw == -1) { bbl->pushInt(-1); return 0; }
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    bbl->pushInt(code);
    return 0;
}

static int bblOs_date(BblState* bbl) {
    const char* fmt = bbl->hasArg(0) ? bbl->getStringArg(0) : "%Y-%m-%d %H:%M:%S";
    time_t t = bbl->hasArg(1) ? (time_t)bbl->getIntArg(1) : ::time(nullptr);
    struct tm* tm = localtime(&t);
    if (!tm) { bbl->pushNull(); return 0; }
    char buf[256];
    strftime(buf, sizeof(buf), fmt, tm);
    bbl->pushString(buf);
    return 0;
}

static int bblOs_difftime(BblState* bbl) {
    bbl->pushFloat(difftime((time_t)bbl->getIntArg(0), (time_t)bbl->getIntArg(1)));
    return 0;
}

static int bblOs_stat(BblState* bbl) {
    struct stat st;
    if (::stat(bbl->getStringArg(0), &st) != 0) { bbl->pushNull(); return 0; }
    BblTable* tbl = bbl->allocTable();
    tbl->set(BblValue::makeString(bbl->intern("size")), BblValue::makeInt((int64_t)st.st_size));
    tbl->set(BblValue::makeString(bbl->intern("mtime")), BblValue::makeInt((int64_t)st.st_mtime));
    tbl->set(BblValue::makeString(bbl->intern("is-dir")), BblValue::makeBool(S_ISDIR(st.st_mode)));
    tbl->set(BblValue::makeString(bbl->intern("is-file")), BblValue::makeBool(S_ISREG(st.st_mode)));
    bbl->pushTable(tbl);
    return 0;
}

static int bblOs_glob(BblState* bbl) {
    glob_t g;
    int ret = ::glob(bbl->getStringArg(0), GLOB_NOSORT, nullptr, &g);
    BblTable* tbl = bbl->allocTable();
    if (ret == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            tbl->set(BblValue::makeInt((int64_t)(i + 1)),
                     BblValue::makeString(bbl->intern(g.gl_pathv[i])));
        }
    }
    if (ret != GLOB_NOMATCH) globfree(&g);
    bbl->pushTable(tbl);
    return 0;
}

static int bblOs_spawn(BblState* bbl) {
    const char* cmd = bbl->getStringArg(0);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw BBL::Error{"spawn failed: " + std::string(cmd)};
    auto* proc = new BblProcess{pipe, false};
    bbl->pushUserData("Process", static_cast<void*>(proc));
    return 0;
}

static int bblProcess_read(BblState* bbl) {
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal->data);
    if (!proc->pipe) { bbl->pushString(""); return 0; }
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), proc->pipe)) result += buf;
    bbl->pushString(result.c_str());
    return 0;
}

static int bblProcess_readLine(BblState* bbl) {
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal->data);
    if (!proc->pipe) { bbl->pushNull(); return 0; }
    std::string result;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), proc->pipe)) {
        bbl->pushNull();
        return 0;
    }
    result = buf;
    while (!result.empty() && result.back() != '\n' && !feof(proc->pipe)) {
        if (!fgets(buf, sizeof(buf), proc->pipe)) break;
        result += buf;
    }
    if (!result.empty() && result.back() == '\n') result.pop_back();
    if (!result.empty() && result.back() == '\r') result.pop_back();
    bbl->pushString(result.c_str());
    return 0;
}

static int bblProcess_wait(BblState* bbl) {
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal->data);
    if (proc->waited) { bbl->pushInt(-1); return 0; }
    int raw = pclose(proc->pipe);
    proc->pipe = nullptr;
    proc->waited = true;
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    bbl->pushInt(code);
    return 0;
}

static int bblOs_spawnDetached(BblState* bbl) {
    const char* cmd = bbl->getStringArg(0);
    pid_t pid = fork();
    if (pid < 0) throw BBL::Error{"spawn-detached: fork failed"};
    if (pid == 0) {
        // Double-fork: first child forks again then exits.
        // Grandchild is reparented to init, preventing zombies.
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0); // first child exits immediately
        // Grandchild: detach session and redirect streams
        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }
    // Reap first child immediately (it exits right away)
    waitpid(pid, nullptr, 0);
    bbl->pushInt((int64_t)pid);
    return 0;
}

void BBL::addOs(BblState& bbl) {
    if (bbl.has("getenv")) return;

    BBL::TypeBuilder pb("Process");
    pb.method("read", bblProcess_read)
      .method("read-line", bblProcess_readLine)
      .method("wait", bblProcess_wait)
      .destructor(processDestructor);
    bbl.registerType(pb);

    bbl.defn("getenv", bblOs_getenv);
    bbl.defn("setenv", bblOs_setenv);
    bbl.defn("unsetenv", bblOs_unsetenv);
    bbl.defn("clock", bblOs_clock);
    bbl.defn("time", bblOs_time);
    bbl.defn("sleep", bblOs_sleep);
    bbl.defn("exit", bblOs_exit);
    bbl.defn("getpid", bblOs_getpid);
    bbl.defn("getcwd", bblOs_getcwd);
    bbl.defn("chdir", bblOs_chdir);
    bbl.defn("mkdir", bblOs_mkdir);
    bbl.defn("remove", bblOs_remove);
    bbl.defn("rename", bblOs_rename);
    bbl.defn("tmpname", bblOs_tmpname);
    bbl.defn("execute", bblOs_execute);
    bbl.defn("date", bblOs_date);
    bbl.defn("difftime", bblOs_difftime);
    bbl.defn("stat", bblOs_stat);
    bbl.defn("glob", bblOs_glob);
    bbl.defn("spawn", bblOs_spawn);
    bbl.defn("spawn-detached", bblOs_spawnDetached);
}

void BBL::addStdLib(BblState& bbl) {
    BBL::addPrint(bbl);
    BBL::addMath(bbl);
    BBL::addFileIo(bbl);
    BBL::addOs(bbl);
    BBL::addChildStates(bbl, false);
}

// ---------- Child States ----------

void MessageQueue::push(BblMessage msg) {
    std::lock_guard lock(mtx);
    messages.push_back(std::move(msg));
    cv.notify_one();
}

BblMessage MessageQueue::pop(std::atomic<bool>& terminated) {
    std::unique_lock lock(mtx);
    cv.wait(lock, [&] { return !messages.empty() || terminated.load(); });
    if (terminated.load()) throw BblTerminated{};
    BblMessage msg = std::move(messages.front());
    messages.pop_front();
    return msg;
}

bool MessageQueue::empty() {
    std::lock_guard lock(mtx);
    return messages.empty();
}

static void stateDestructor(void* data) {
    auto* handle = static_cast<BblStateHandle*>(data);
    if (!handle) return;
    handle->child->terminated.store(true);
    { std::lock_guard lk(handle->toChild.mtx); }
    handle->toChild.cv.notify_one();
    { std::lock_guard lk(handle->toParent.mtx); }
    handle->toParent.cv.notify_one();
    if (handle->thread.joinable()) handle->thread.join();
    delete handle->child;
    delete handle;
}

// Forward declarations for child-side functions
static int bblChildPost(BblState* bbl);
static int bblChildRecv(BblState* bbl);
static int bblChildRecvVec(BblState* bbl);

static BblMessage serializeMessage(BblState* bbl, BblTable* table, BblValue* vecArg) {
    BblMessage msg;
    for (auto& k : table->order) {
        if (k.type != BBL::Type::String)
            throw BBL::Error{"message key must be a string"};
        auto val = table->get(k).value_or(BblValue::makeNull());
        MessageValue mv;
        mv.type = val.type;
        switch (val.type) {
            case BBL::Type::Int:    mv.intVal = val.intVal; break;
            case BBL::Type::Float:  mv.floatVal = val.floatVal; break;
            case BBL::Type::Bool:   mv.boolVal = val.boolVal; break;
            case BBL::Type::Null:   break;
            case BBL::Type::String: mv.stringVal = val.stringVal->data; break;
            default:
                throw BBL::Error{"message value must be int, float, bool, null, or string"};
        }
        msg.entries.emplace_back(k.stringVal->data, std::move(mv));
    }
    if (vecArg) {
        if (vecArg->type == BBL::Type::Vector) {
            msg.hasPayload = true;
            msg.payloadData = std::move(vecArg->vectorVal->data);
            msg.payloadElemType = vecArg->vectorVal->elemType;
            msg.payloadElemTypeTag = vecArg->vectorVal->elemTypeTag;
            msg.payloadElemSize = vecArg->vectorVal->elemSize;
        } else if (vecArg->type == BBL::Type::Binary) {
            msg.hasPayload = true;
            msg.payloadData = std::move(vecArg->binaryVal->data);
        } else {
            throw BBL::Error{"post payload must be a vector or binary"};
        }
    }
    return msg;
}

static BblValue deserializeMessage(BblState* bbl, BblMessage& msg) {
    GcPauseGuard guard(bbl);
    BblTable* table = bbl->allocTable();
    for (auto& entry : msg.entries) {
        BblString* key = bbl->intern(entry.first);
        BblValue val;
        auto& mv = entry.second;
        switch (mv.type) {
            case BBL::Type::Int:    val = BblValue::makeInt(mv.intVal); break;
            case BBL::Type::Float:  val = BblValue::makeFloat(mv.floatVal); break;
            case BBL::Type::Bool:   val = BblValue::makeBool(mv.boolVal); break;
            case BBL::Type::Null:   val = BblValue::makeNull(); break;
            case BBL::Type::String: val = BblValue::makeString(bbl->intern(mv.stringVal)); break;
            default: break;
        }
        table->set(BblValue::makeString(key), val);
    }
    if (msg.hasPayload) {
        if (!msg.payloadElemType.empty()) {
            BblVec* vec = bbl->allocVector(msg.payloadElemType, msg.payloadElemTypeTag, msg.payloadElemSize);
            vec->data = std::move(msg.payloadData);
            bbl->lastRecvPayload = BblValue::makeVector(vec);
        } else {
            BblBinary* bin = bbl->allocBinary(std::move(msg.payloadData));
            bbl->lastRecvPayload = BblValue::makeBinary(bin);
        }
    } else {
        bbl->lastRecvPayload = BblValue::makeNull();
    }
    return BblValue::makeTable(table);
}

static int bblStateNew(BblState* bbl) {
    std::string path = bbl->getStringArg(0);
    auto* child = new BblState();
    child->allowOpenFilesystem = true;
    BBL::addStdLib(*child);
    // addStdLib already registered State type + state-new/state-destroy.
    // Now add child-mode flat functions (post, recv, recv-vec).
    child->defn("post", bblChildPost);
    child->defn("recv", bblChildRecv);
    child->defn("recv-vec", bblChildRecvVec);
    auto* handle = new BblStateHandle();
    handle->child = child;
    child->handle = handle;
    BblUserData* ud = bbl->allocUserData("State", handle);
    handle->thread = std::thread([handle, path] {
        BblState* child = handle->child;
        try {
            child->execfile(path);
        } catch (const BblTerminated&) {
            // Normal termination
        } catch (const BBL::Error& e) {
            handle->childError = e.what;
        } catch (const std::exception& e) {
            handle->childError = e.what();
        } catch (...) {
            handle->childError = "unknown error in child thread";
        }
        handle->done.store(true, std::memory_order_release);
        { std::lock_guard lk(handle->toParent.mtx); }
        handle->toParent.cv.notify_one();
    });
    bbl->returnValue = BblValue::makeUserData(ud);
    bbl->hasReturn = true;
    return 0;
}

// --- Parent-side methods ---

static BblStateHandle* getHandle(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData || !self.userdataVal->data)
        throw BBL::Error{"state handle is invalid (destroyed)"};
    return static_cast<BblStateHandle*>(self.userdataVal->data);
}

static int statePost(BblState* bbl) {
    auto* handle = getHandle(bbl);
    BblValue tblArg = bbl->getArg(1);
    if (tblArg.type != BBL::Type::Table)
        throw BBL::Error{"post: first argument must be a table"};
    BblValue* vecArg = nullptr;
    BblValue vecVal;
    if (bbl->argCount() >= 3) {
        vecVal = bbl->getArg(2);
        vecArg = &vecVal;
    }
    BblMessage msg = serializeMessage(bbl, tblArg.tableVal, vecArg);
    handle->toChild.push(std::move(msg));
    return 0;
}

static int stateRecv(BblState* bbl) {
    auto* handle = getHandle(bbl);
    std::unique_lock lock(handle->toParent.mtx);
    handle->toParent.cv.wait(lock, [&] {
        return !handle->toParent.messages.empty()
            || bbl->terminated.load()
            || handle->done.load(std::memory_order_acquire);
    });
    if (!handle->toParent.messages.empty()) {
        BblMessage msg = std::move(handle->toParent.messages.front());
        handle->toParent.messages.pop_front();
        lock.unlock();
        BblValue table = deserializeMessage(bbl, msg);
        bbl->returnValue = table;
        bbl->hasReturn = true;
        return 0;
    }
    lock.unlock();
    if (bbl->terminated.load()) throw BblTerminated{};
    if (handle->childError) throw BBL::Error{*handle->childError};
    throw BBL::Error{"child state has exited"};
}

static int stateRecvVec(BblState* bbl) {
    getHandle(bbl);  // validate handle
    bbl->returnValue = bbl->lastRecvPayload;
    bbl->hasReturn = true;
    return 0;
}

static int stateJoin(BblState* bbl) {
    auto* handle = getHandle(bbl);
    if (handle->thread.joinable()) handle->thread.join();
    return 0;
}

static int stateIsDone(BblState* bbl) {
    auto* handle = getHandle(bbl);
    bbl->pushBool(handle->done.load(std::memory_order_acquire));
    return 0;
}

static int stateHasError(BblState* bbl) {
    auto* handle = getHandle(bbl);
    if (!handle->done.load(std::memory_order_acquire)) { bbl->pushBool(false); return 0; }
    bbl->pushBool(handle->childError.has_value());
    return 0;
}

static int stateGetError(BblState* bbl) {
    auto* handle = getHandle(bbl);
    if (!handle->done.load(std::memory_order_acquire)) { bbl->pushNull(); return 0; }
    if (handle->childError) {
        bbl->pushString(handle->childError->c_str());
    } else {
        bbl->pushNull();
    }
    return 0;
}

static int stateDestroy(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type != BBL::Type::UserData || !self.userdataVal->data)
        throw BBL::Error{"state handle is invalid (destroyed)"};
    auto* handle = static_cast<BblStateHandle*>(self.userdataVal->data);
    stateDestructor(handle);
    self.userdataVal->data = nullptr;
    return 0;
}

// --- Child-side flat functions ---

static int bblChildPost(BblState* bbl) {
    auto* handle = bbl->handle;
    if (!handle) throw BBL::Error{"post: not inside a child state"};
    BblValue tblArg = bbl->getArg(0);
    if (tblArg.type != BBL::Type::Table)
        throw BBL::Error{"post: first argument must be a table"};
    BblValue* vecArg = nullptr;
    BblValue vecVal;
    if (bbl->argCount() >= 2) {
        vecVal = bbl->getArg(1);
        vecArg = &vecVal;
    }
    BblMessage msg = serializeMessage(bbl, tblArg.tableVal, vecArg);
    handle->toParent.push(std::move(msg));
    return 0;
}

static int bblChildRecv(BblState* bbl) {
    auto* handle = bbl->handle;
    if (!handle) throw BBL::Error{"recv: not inside a child state"};
    BblMessage msg = handle->toChild.pop(bbl->terminated);
    BblValue table = deserializeMessage(bbl, msg);
    bbl->returnValue = table;
    bbl->hasReturn = true;
    return 0;
}

static int bblChildRecvVec(BblState* bbl) {
    if (!bbl->handle) throw BBL::Error{"recv-vec: not inside a child state"};
    bbl->returnValue = bbl->lastRecvPayload;
    bbl->hasReturn = true;
    return 0;
}

// --- Registration ---

void BBL::addChildStates(BblState& bbl, bool) {
    BBL::TypeBuilder sb("State");
    sb.method("post", statePost)
      .method("recv", stateRecv)
      .method("recv-vec", stateRecvVec)
      .method("join", stateJoin)
      .method("is-done", stateIsDone)
      .method("has-error", stateHasError)
      .method("get-error", stateGetError);
    sb.destructor(stateDestructor);
    bbl.registerType(sb);
    bbl.defn("state-new", bblStateNew);
    bbl.defn("state-destroy", stateDestroy);
}
