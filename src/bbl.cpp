#include "bbl.h"
#include <yyjson.h>
#include "compat.h"
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
#include <sys/stat.h>
#include "compiler.h"
#include "vm.h"
#include "jit.h"

// ---------- BblValue ----------

static bool bblValueKeyEqual(const BblValue& a, const BblValue& b) {
    if (a.type() != b.type()) return false;
    if (a.type() == BBL::Type::String) return a.stringVal() == b.stringVal();
    if (a.type() == BBL::Type::Int) return a.intVal() == b.intVal();
    if (a.type() == BBL::Type::Float) return a.floatVal() == b.floatVal();
    if (a.type() == BBL::Type::Bool) return a.boolVal() == b.boolVal();
    return false;
}

// ---------- BblTable ----------

static size_t hashValue(const BblValue& v) {
    return std::hash<uint64_t>{}(v.bits);
}

static BblTable::Entry* tableFindEntry(BblTable::Entry* buckets, size_t cap, const BblValue& key) {
    size_t idx = hashValue(key) & (cap - 1);
    BblTable::Entry* firstTombstone = nullptr;
    for (size_t i = 0; i < cap; i++) {
        BblTable::Entry& e = buckets[(idx + i) & (cap - 1)];
        if (e.isEmpty()) {
            return firstTombstone ? firstTombstone : &e;
        }
        if (e.isTombstone()) {
            if (!firstTombstone) firstTombstone = &e;
            continue;
        }
        if (bblValueKeyEqual(e.key, key)) return &e;
    }
    return firstTombstone;
}

static void tableGrow(BblTable* tbl) {
    size_t newCap = tbl->capacity < 8 ? 8 : tbl->capacity * 2;
    auto* newBuckets = static_cast<BblTable::Entry*>(calloc(newCap, sizeof(BblTable::Entry)));
    for (uint32_t i = 0; i < tbl->capacity; i++) {
        auto& e = tbl->buckets[i];
        if (e.isOccupied()) {
            auto* dest = tableFindEntry(newBuckets, newCap, e.key);
            dest->key = e.key;
            dest->val = e.val;
            
        }
    }
    if (tbl->buckets != tbl->inlineBuckets) free(tbl->buckets);
    tbl->buckets = newBuckets;
    tbl->capacity = static_cast<uint32_t>(newCap);
}

std::expected<BblValue, BBL::GetError> BblTable::get(const BblValue& key) const {
    if (count == 0 || capacity == 0) return std::unexpected(BBL::GetError::NotFound);
    auto* e = tableFindEntry(buckets, capacity, key);
    if (!e || !e->isOccupied()) return std::unexpected(BBL::GetError::NotFound);
    return e->val;
}

void BblTable::set(const BblValue& key, const BblValue& val) {
    if (capacity == 0 || count + 1 > capacity * 3 / 4) tableGrow(this);
    auto* e = tableFindEntry(buckets, capacity, key);
    bool isNew = !e->isOccupied();
    e->key = key;
    e->val = val;
    
    if (isNew) {
        count++;
        if (!order) order = new std::vector<BblValue>();
        order->push_back(key);
    }
    if (key.type() == BBL::Type::Int && key.intVal() >= nextIntKey)
        nextIntKey = key.intVal() + 1;
}

bool BblTable::has(const BblValue& key) const {
    if (count == 0 || capacity == 0) return false;
    auto* e = tableFindEntry(buckets, capacity, key);
    return e && e->isOccupied();
}

bool BblTable::del(const BblValue& key) {
    if (count == 0 || capacity == 0) return false;
    auto* e = tableFindEntry(buckets, capacity, key);
    if (!e || !e->isOccupied()) return false;
    e->key.bits = BblTable::TOMBSTONE_KEY;
    e->val = BblValue::makeNull();
    count--;
    if (order) {
        for (auto it = order->begin(); it != order->end(); ++it) {
            if (bblValueKeyEqual(*it, key)) { order->erase(it); break; }
        }
    }
    return true;
}

// ---------- BblValue eq ----------

bool BblValue::operator==(const BblValue& o) const {
    if (isDouble() && o.isDouble()) return floatVal() == o.floatVal();
    return bits == o.bits;
}

// ---------- BblScope ----------


uint32_t BblState::resolveSymbol(const std::string& name) const {
    auto it = symbolIds.find(name);
    if (it != symbolIds.end()) return it->second;
    uint32_t id = nextSymbolId++;
    symbolIds[name] = id;
    return id;
}

// ---------- Lexer ----------

BblLexer::BblLexer(const char* source) : src(source), len(static_cast<int>(strlen(source))) {}
BblLexer::BblLexer(const char* source, size_t length) : src(source), len(static_cast<int>(length)) {}

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
    if (!isNegative && peek() == '0' && pos + 1 < len && src[pos + 1] == 'z') {
        return readCompressedBinary();
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
    t.binarySource = src + pos;
    t.binarySize = size;
    t.line = startLine;
    for (size_t i = 0; i < size; i++) {
        if (src[pos] == '\n') line++;
        pos++;
    }
    return t;
}

Token BblLexer::readCompressedBinary() {
    int startLine = line;
    pos += 2; // skip 0z
    int sizeStart = pos;
    while (pos < len && peek() >= '0' && peek() <= '9') pos++;
    if (pos >= len || peek() != ':')
        throw BBL::Error{"invalid compressed binary literal at line " + std::to_string(startLine)};
    size_t size = std::stoull(std::string(src + sizeStart, src + pos));
    pos++; // skip :
    if (static_cast<size_t>(len - pos) < size)
        throw BBL::Error{"compressed binary literal: insufficient bytes at line " + std::to_string(startLine)};
    Token t;
    t.type = TokenType::Binary;
    t.binarySource = src + pos;
    t.binarySize = size;
    t.isCompressed = true;
    t.line = startLine;
    for (size_t i = 0; i < size; i++) {
        if (src[pos] == '\n') line++;
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
        t.sourceStart = pos;
        t.sourceEnd = pos;
        return t;
    }

    int startP = pos;
    char c = peek();

    if (c == '(') {
        advance();
        return {TokenType::LParen, 0, 0, false, "", {}, nullptr, 0, false, line, startP, pos};
    }
    if (c == ')') {
        advance();
        return {TokenType::RParen, 0, 0, false, "", {}, nullptr, 0, false, line, startP, pos};
    }
    if (c == '"') {
        Token t = readString();
        t.sourceStart = startP; t.sourceEnd = pos;
        return t;
    }

    if (c == '-' && pos + 1 < len && src[pos + 1] >= '0' && src[pos + 1] <= '9') {
        Token t = readNumber();
        t.sourceStart = startP; t.sourceEnd = pos;
        return t;
    }

    if ((c >= '0' && c <= '9')) {
        Token t = readNumber();
        t.sourceStart = startP; t.sourceEnd = pos;
        return t;
    }

    if (c == '-' || isOperatorChar(c)) {
        Token t = readSymbolOrKeyword();
        t.sourceStart = startP; t.sourceEnd = pos;
        return t;
    }

    if (c == '.') {
        advance();
        return {TokenType::Dot, 0, 0, false, "", {}, nullptr, 0, false, line, startP, pos};
    }

    if (c == ':') {
        advance();
        return {TokenType::Colon, 0, 0, false, "", {}, nullptr, 0, false, line, startP, pos};
    }

    if (isSymbolStart(c)) {
        Token t = readSymbolOrKeyword();
        t.sourceStart = startP; t.sourceEnd = pos;
        return t;
    }

    throw BBL::Error{"unexpected character '" + std::string(1, c) + "' at line " + std::to_string(line)};
}

// ---------- Parser ----------

static AstNode parseExpr(BblLexer& lexer, Token& tok);

static AstNode parsePrimary(BblLexer& lexer, Token& tok) {
    AstNode node;
    node.line = tok.line;

    switch(tok.type) {
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
            node.binarySource = tok.binarySource;
            node.binarySize = tok.binarySize;
            node.isCompressed = tok.isCompressed;
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
    vm = std::make_unique<VmState>();

    m.length = intern("length"); m.push = intern("push"); m.pop = intern("pop");
    m.clear = intern("clear"); m.at = intern("at"); m.set = intern("set");
    m.get = intern("get"); m.resize = intern("resize"); m.reserve = intern("reserve"); m.has = intern("has");
    m.del = intern("delete"); m.keys = intern("keys"); m.find = intern("find");
    m.contains = intern("contains"); m.starts_with = intern("starts-with");
    m.ends_with = intern("ends-with"); m.slice = intern("slice");
    m.split = intern("split"); m.replace = intern("replace");
    m.upper = intern("upper"); m.lower = intern("lower"); m.trim = intern("trim");
    m.copy_from = intern("copy-from");
    m.join = intern("join");
    m.trim_left = intern("trim-left"); m.trim_right = intern("trim-right");
    m.pad_left = intern("pad-left"); m.pad_right = intern("pad-right");

    m.length->methodId = MID_LENGTH; m.push->methodId = MID_PUSH; m.pop->methodId = MID_POP;
    m.clear->methodId = MID_CLEAR; m.at->methodId = MID_AT; m.set->methodId = MID_SET;
    m.get->methodId = MID_GET; m.resize->methodId = MID_RESIZE; m.reserve->methodId = MID_RESERVE;
    m.has->methodId = MID_HAS; m.del->methodId = MID_DEL; m.keys->methodId = MID_KEYS;
    m.find->methodId = MID_FIND; m.contains->methodId = MID_CONTAINS;
    m.starts_with->methodId = MID_STARTS_WITH; m.ends_with->methodId = MID_ENDS_WITH;
    m.slice->methodId = MID_SLICE; m.split->methodId = MID_SPLIT;
    m.replace->methodId = MID_REPLACE; m.upper->methodId = MID_UPPER;
    m.lower->methodId = MID_LOWER; m.trim->methodId = MID_TRIM;
    m.copy_from->methodId = MID_COPY_FROM; m.join->methodId = MID_JOIN;
    m.trim_left->methodId = MID_TRIM_LEFT; m.trim_right->methodId = MID_TRIM_RIGHT;
    m.pad_left->methodId = MID_PAD_LEFT; m.pad_right->methodId = MID_PAD_RIGHT;
    m.as = intern("as"); m.set_as = intern("set-as");
    m.as->methodId = MID_AS; m.set_as->methodId = MID_SET_AS;

    defn("__with_cleanup", [](BblState* bbl) -> int {
        if (bbl->callArgs.empty()) return 0;
        BblValue& val = bbl->callArgs[0];
        if (val.type() == BBL::Type::UserData && val.userdataVal()->desc &&
            val.userdataVal()->desc->destructor && val.userdataVal()->data) {
            val.userdataVal()->desc->destructor(val.userdataVal()->data);
            val.userdataVal()->data = nullptr;
        }
        return 0;
    });

    defn("__with_rethrow", [](BblState* bbl) -> int {
        if (!bbl->callArgs.empty() && bbl->callArgs[0].type() == BBL::Type::String)
            throw BBL::Error{bbl->callArgs[0].stringVal()->data};
        throw BBL::Error{"unknown error in with block"};
    });

    defn("__with_typecheck", [](BblState* bbl) -> int {
        if (bbl->callArgs.empty() || bbl->callArgs[0].type() != BBL::Type::UserData)
            throw BBL::Error{"with: initializer must produce userdata"};
        return 0;
    });
}

BblState::~BblState() {
    GcObj* obj = gcHead;
    while (obj) {
        GcObj* next = obj->gcNext;
        switch (obj->gcType) {
            case GcType::String:  delete static_cast<BblString*>(obj); break;
            case GcType::Binary:  delete static_cast<BblBinary*>(obj); break;
            case GcType::Fn:      delete static_cast<BblFn*>(obj); break;
            case GcType::Struct:  delete static_cast<BblStruct*>(obj); break;
            case GcType::Vec:     delete static_cast<BblVec*>(obj); break;
            case GcType::Table:   static_cast<BblTable*>(obj)->~BblTable(); break;
            case GcType::UserData: {
                auto* u = static_cast<BblUserData*>(obj);
                if (u->desc && u->desc->destructor && u->data) u->desc->destructor(u->data);
                delete u;
                break;
            }
            case GcType::Closure: {
                auto* c = static_cast<BblClosure*>(obj);
                if (c->chunk.traceCode) jitFree(c->chunk.traceCode, c->chunk.traceCapacity);
                delete c;
                break;
            }
        }
        obj = next;
    }
    gcHead = nullptr;
}

BblString* BblState::intern(const std::string& s) {
    auto it = internTable.find(s);
    if (it != internTable.end()) {
        return it->second;
    }
    auto* str = new BblString{s, false, true};
    str->gcNext = gcHead; gcHead = str;
    internTable[str->data] = str;
    allocCount++;
    return str;
}

BblString* BblState::allocString(std::string s) {
    auto* str = new BblString{std::move(s)};
    str->gcNext = gcHead; gcHead = str;
    allocCount++;
    return str;
}

#include <lz4frame.h>

void BblBinary::materialize() {
    if (!lazySource) return;
    if (compressed) {
        data = BBL::lz4Decompress(reinterpret_cast<const uint8_t*>(lazySource), lazySize);
    } else {
        data.assign(reinterpret_cast<const uint8_t*>(lazySource),
                    reinterpret_cast<const uint8_t*>(lazySource) + lazySize);
    }
    lazySource = nullptr;
}

size_t BblBinary::length() const {
    if (!lazySource) return data.size();
    if (!compressed) return lazySize;
    LZ4F_dctx* ctx = nullptr;
    LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
    LZ4F_frameInfo_t info = {};
    size_t consumed = lazySize;
    size_t ret = LZ4F_getFrameInfo(ctx, &info, lazySource, &consumed);
    LZ4F_freeDecompressionContext(ctx);
    if (LZ4F_isError(ret) || !info.contentSize)
        throw BBL::Error{"compressed binary: invalid LZ4 frame"};
    return static_cast<size_t>(info.contentSize);
}

BblBinary* BblState::allocBinary(std::vector<uint8_t> data) {
    auto* b = new BblBinary();
    b->data = std::move(data);
    b->gcNext = gcHead; gcHead = b;
    allocCount++;
    return b;
}

BblBinary* BblState::allocLazyBinary(const char* src, size_t size, bool isCompressed) {
    auto* b = new BblBinary();
    b->lazySource = src;
    b->lazySize = size;
    b->compressed = isCompressed;
    b->gcNext = gcHead; gcHead = b;
    allocCount++;
    return b;
}

BblFn* BblState::allocFn() {
    auto* f = new BblFn{};
    f->gcNext = gcHead; gcHead = f;
    allocCount++;
    return f;
}

BblStruct* BblState::allocStruct(StructDesc* desc) {
    auto* s = new BblStruct();
    s->desc = desc;
    s->data.resize(desc->totalSize, 0);
    s->gcNext = gcHead; gcHead = s;
    allocCount++;
    return s;
}

BblVec* BblState::allocVector(const std::string& elemType, BBL::Type elemTypeTag, size_t elemSize) {
    auto* v = new BblVec();
    v->elemType = elemType;
    v->elemTypeTag = elemTypeTag;
    v->elemSize = elemSize;
    v->gcNext = gcHead; gcHead = v;
    allocCount++;
    return v;
}

BblTable* BblState::allocTable() {
    BblTable* t = tableSlab.alloc();
    t->gcNext = gcHead; gcHead = t;
    allocCount++;
    return t;
}

BblUserData* BblState::allocUserData(const std::string& typeName, void* data) {
    auto it = userDataDescs.find(typeName);
    if (it == userDataDescs.end()) {
        throw BBL::Error{"unknown userdata type: " + typeName};
    }
    auto* u = new BblUserData();
    u->desc = &it->second;
    u->data = data;
    u->gcNext = gcHead; gcHead = u;
    allocCount++;
    return u;
}

// ---------- GC ----------

static void gcMark(BblValue& val);


static void gcMark(BblValue& val) {
    switch (val.type()) {
        case BBL::Type::Binary:
            if (val.binaryVal() && !val.binaryVal()->marked) {
                val.binaryVal()->marked = true;
            }
            break;
        case BBL::Type::Fn:
            if (val.isClosure() && val.closureVal() && !val.closureVal()->marked) {
                val.closureVal()->marked = true;
                for (auto& cap : val.closureVal()->captures) {
                    gcMark(cap);
                }
                if (val.closureVal()->env && !val.closureVal()->env->marked) {
                    BblValue envVal = BblValue::makeTable(val.closureVal()->env);
                    gcMark(envVal);
                }
            } else if (!val.isCFn() && !val.isClosure() && val.fnVal() && !val.fnVal()->marked) {
                val.fnVal()->marked = true;
                for (auto& [name, cap] : val.fnVal()->captures) {
                    gcMark(cap);
                }
            }
            break;
        case BBL::Type::Struct:
            if (val.structVal() && !val.structVal()->marked) {
                val.structVal()->marked = true;
            }
            break;
        case BBL::Type::Vector:
            if (val.vectorVal() && !val.vectorVal()->marked) {
                val.vectorVal()->marked = true;
            }
            break;
        case BBL::Type::Table:
            if (val.tableVal() && !val.tableVal()->marked) {
                val.tableVal()->marked = true;
                for (uint32_t i = 0; i < val.tableVal()->capacity; i++) {
                    auto& e = val.tableVal()->buckets[i];
                    if (e.isOccupied()) {
                        BblValue km = e.key;
                        BblValue vm = e.val;
                        gcMark(km);
                        gcMark(vm);
                    }
                }
            }
            break;
        case BBL::Type::UserData:
            if (val.userdataVal() && !val.userdataVal()->marked) {
                val.userdataVal()->marked = true;
            }
            break;
        case BBL::Type::String:
            if (val.stringVal() && !val.stringVal()->marked) {
                val.stringVal()->marked = true;
            }
            break;
        default:
            break;
    }
}

void BblState::gc() {
    // Mark phase
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
            if (vm->frames[i].regs[0].closureVal()) {
                BblValue cv = BblValue::makeClosure(vm->frames[i].regs[0].closureVal());
                gcMark(cv);
            }
        }
        for (auto& [id, val] : vm->globals)
            gcMark(val);
    }

    if (currentEnv && !currentEnv->marked) {
        BblValue envVal = BblValue::makeTable(currentEnv);
        gcMark(envVal);
    }
    for (auto& [path, env] : moduleCache) {
        if (env && !env->marked) {
            BblValue envVal = BblValue::makeTable(env);
            gcMark(envVal);
        }
    }

    // Sweep: walk linked list, free unmarked, clear marks on survivors
    size_t liveCount = 0;
    GcObj** p = &gcHead;
    while (*p) {
        if (!(*p)->marked) {
            GcObj* dead = *p;
            *p = dead->gcNext;
            switch (dead->gcType) {
                case GcType::String:
                    internTable.erase(static_cast<BblString*>(dead)->data);
                    delete static_cast<BblString*>(dead);
                    break;
                case GcType::Table:
                    tableSlab.free(static_cast<BblTable*>(dead));
                    break;
                case GcType::Closure: {
                    auto* c = static_cast<BblClosure*>(dead);
                    if (c->chunk.traceCode) jitFree(c->chunk.traceCode, c->chunk.traceCapacity);
                    delete c;
                    break;
                }
                case GcType::UserData: {
                    auto* u = static_cast<BblUserData*>(dead);
                    if (u->desc && u->desc->destructor && u->data) u->desc->destructor(u->data);
                    delete u;
                    break;
                }
                case GcType::Binary:  delete static_cast<BblBinary*>(dead); break;
                case GcType::Fn:      delete static_cast<BblFn*>(dead); break;
                case GcType::Struct:  delete static_cast<BblStruct*>(dead); break;
                case GcType::Vec:     delete static_cast<BblVec*>(dead); break;
            }
        } else {
            (*p)->marked = false;
            p = &(*p)->gcNext;
            liveCount++;
        }
    }

    gcThreshold = std::max<size_t>(4096, liveCount * 2);
    allocCount = 0;
    memset(sliceCache, 0, sizeof(sliceCache));
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

// ---------- exec / execfile ----------

void BblState::materializeLazyBinaries() {
    for (GcObj* obj = gcHead; obj; obj = obj->gcNext) {
        if (obj->gcType == GcType::Binary) {
            static_cast<BblBinary*>(obj)->materialize();
        }
    }
}

void BblState::exec(const std::string& source) {
    BblLexer lexer(source.c_str(), source.size());
    auto nodes = parse(lexer);
    Chunk chunk = compile(*this, nodes);
    jitExecute(*this, chunk);
    materializeLazyBinaries();
}

BblValue BblState::execExpr(const std::string& source) {
    BblLexer lexer(source.c_str(), source.size());
    auto nodes = parse(lexer);
    Chunk chunk = compile(*this, nodes);
    auto result = jitExecute(*this, chunk);
    materializeLazyBinaries();
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
    if (++execDepth > MAX_EXEC_DEPTH)
        throw BBL::Error{"execfile: recursion depth exceeded (max " + std::to_string(MAX_EXEC_DEPTH) + ")"};
    namespace fs = std::filesystem;
    fs::path resolved = resolveSandboxPath(path, "exec-file");
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
    std::ifstream file(resolved, std::ios::binary);
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
    execDepth--;
}

BblValue BblState::execfileExpr(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path resolved = resolveSandboxPath(path, "exec-file");
    if (!fs::exists(resolved)) {
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
                    if (fs::exists(candidate)) { resolved = candidate; break; }
                }
                pos = sep + 1;
            }
        }
    }
    if (++execDepth > MAX_EXEC_DEPTH)
        throw BBL::Error{"execfile: recursion depth exceeded"};
    std::ifstream file(resolved, std::ios::binary);
    if (!file.is_open()) throw BBL::Error{"file read failed: " + resolved.string()};
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string savedFile = currentFile;
    std::string savedDir = scriptDir;
    currentFile = resolved.string();
    scriptDir = resolved.parent_path().string();
    BblValue result = execExpr(ss.str());
    currentFile = savedFile;
    scriptDir = savedDir;
    execDepth--;
    return result;
}

// ---------- Introspection ----------

bool BblState::has(const std::string& name) const {
    uint32_t id = resolveSymbol(name);
    return vm->globals.count(id) > 0;
}

std::expected<BBL::Type, BBL::GetError> BblState::getType(const std::string& name) const {
    uint32_t id = resolveSymbol(name);
    auto it = vm->globals.find(id);
    if (it == vm->globals.end()) {
        return std::unexpected(BBL::GetError::NotFound);
    }
    return it->second.type();
}

std::expected<BblValue, BBL::GetError> BblState::get(const std::string& name) const {
    uint32_t id = resolveSymbol(name);
    auto it = vm->globals.find(id);
    if (it == vm->globals.end()) {
        return std::unexpected(BBL::GetError::NotFound);
    }
    return it->second;
}

std::expected<int64_t, BBL::GetError> BblState::getInt(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::Int) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->intVal();
}

std::expected<double, BBL::GetError> BblState::getFloat(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::Float) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->floatVal();
}

std::expected<bool, BBL::GetError> BblState::getBool(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::Bool) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->boolVal();
}

std::expected<const char*, BBL::GetError> BblState::getString(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::String) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->stringVal()->data.c_str();
}

std::expected<BblTable*, BBL::GetError> BblState::getTable(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::Table) return std::unexpected(BBL::GetError::TypeMismatch);
    return v->tableVal();
}

std::expected<BblBinary*, BBL::GetError> BblState::getBinary(const std::string& name) const {
    auto v = get(name);
    if (!v) return std::unexpected(v.error());
    if (v->type() != BBL::Type::Binary) return std::unexpected(BBL::GetError::TypeMismatch);
    v->binaryVal()->materialize();
    return v->binaryVal();
}

// ---------- Setters ----------

void BblState::setInt(const std::string& name, int64_t val) {
    vm->globals[resolveSymbol(name)] = BblValue::makeInt(val);
}

void BblState::setFloat(const std::string& name, double val) {
    vm->globals[resolveSymbol(name)] = BblValue::makeFloat(val);
}

void BblState::setString(const std::string& name, const char* str) {
    vm->globals[resolveSymbol(name)] = BblValue::makeString(intern(str));
}

void BblState::set(const std::string& name, BblValue val) {
    vm->globals[resolveSymbol(name)] = val;
}

void BblState::setBinary(const std::string& name, const uint8_t* ptr, size_t size) {
    std::vector<uint8_t> data(ptr, ptr + size);
    vm->globals[resolveSymbol(name)] = BblValue::makeBinary(allocBinary(std::move(data)));
}

void BblState::pushUserData(const std::string& typeName, void* ptr) {
    returnValue = BblValue::makeUserData(allocUserData(typeName, ptr));
    hasReturn = true;
}

// ---------- C function registration ----------

void BblState::defn(const std::string& name, BblCFunction fn) {
    vm->globals[resolveSymbol(name)] = BblValue::makeCFn(fn);
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
    return callArgs[i].type();
}

int64_t BblState::getIntArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type() != BBL::Type::Int) {
        throw BBL::Error{"type mismatch: expected int arg, got " + typeName(callArgs[i].type())};
    }
    return callArgs[i].intVal();
}

double BblState::getFloatArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type() != BBL::Type::Float) {
        throw BBL::Error{"type mismatch: expected float arg, got " + typeName(callArgs[i].type())};
    }
    return callArgs[i].floatVal();
}

bool BblState::getBoolArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type() != BBL::Type::Bool) {
        throw BBL::Error{"type mismatch: expected bool arg, got " + typeName(callArgs[i].type())};
    }
    return callArgs[i].boolVal();
}

const char* BblState::getStringArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type() != BBL::Type::String) {
        throw BBL::Error{"type mismatch: expected string arg, got " + typeName(callArgs[i].type())};
    }
    return callArgs[i].stringVal()->data.c_str();
}

BblBinary* BblState::getBinaryArg(int i) const {
    if (!hasArg(i)) {
        throw BBL::Error{"argument index " + std::to_string(i) + " out of bounds"};
    }
    if (callArgs[i].type() != BBL::Type::Binary) {
        throw BBL::Error{"type mismatch: expected binary arg, got " + typeName(callArgs[i].type())};
    }
    auto* bin = callArgs[i].binaryVal();
    bin->materialize();
    return bin;
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
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            int8_t v = static_cast<int8_t>(val.intVal());
            memcpy(p, &v, 1);
            return;
        }
        case CType::Uint8: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            uint8_t v = static_cast<uint8_t>(val.intVal());
            memcpy(p, &v, 1);
            return;
        }
        case CType::Int16: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            int16_t v = static_cast<int16_t>(val.intVal());
            memcpy(p, &v, 2);
            return;
        }
        case CType::Uint16: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            uint16_t v = static_cast<uint16_t>(val.intVal());
            memcpy(p, &v, 2);
            return;
        }
        case CType::Float32: {
            if (val.type() == BBL::Type::Float) {
                float v = static_cast<float>(val.floatVal());
                memcpy(p, &v, sizeof(float));
            } else if (val.type() == BBL::Type::Int) {
                float v = static_cast<float>(val.intVal());
                memcpy(p, &v, sizeof(float));
            } else {
                throw BBL::Error{"type mismatch: expected numeric, got " + typeName(val.type())};
            }
            return;
        }
        case CType::Float64: {
            if (val.type() == BBL::Type::Float) {
                auto tmp = val.floatVal();
                memcpy(p, &tmp, sizeof(double));
            } else if (val.type() == BBL::Type::Int) {
                double v = static_cast<double>(val.intVal());
                memcpy(p, &v, sizeof(double));
            } else {
                throw BBL::Error{"type mismatch: expected numeric, got " + typeName(val.type())};
            }
            return;
        }
        case CType::Int32: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            int32_t v = static_cast<int32_t>(val.intVal());
            memcpy(p, &v, sizeof(int32_t));
            return;
        }
        case CType::Uint32: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            uint32_t v = static_cast<uint32_t>(val.intVal());
            memcpy(p, &v, sizeof(uint32_t));
            return;
        }
        case CType::Int64: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            { auto tmp = val.intVal(); memcpy(p, &tmp, sizeof(int64_t)); }
            return;
        }
        case CType::Uint64: {
            if (val.type() != BBL::Type::Int) {
                throw BBL::Error{"type mismatch: expected int, got " + typeName(val.type())};
            }
            uint64_t v = static_cast<uint64_t>(val.intVal());
            memcpy(p, &v, sizeof(uint64_t));
            return;
        }
        case CType::Bool: {
            if (val.type() != BBL::Type::Bool) {
                throw BBL::Error{"type mismatch: expected bool, got " + typeName(val.type())};
            }
            uint8_t v = val.boolVal() ? 1 : 0;
            *p = v;
            return;
        }
        case CType::Struct: {
            if (val.type() != BBL::Type::Struct || val.structVal()->desc->name != fd.structType) {
                throw BBL::Error{"type mismatch: expected struct " + fd.structType};
            }
            memcpy(p, val.structVal()->data.data(), fd.size);
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
            if (val.type() != BBL::Type::Int)
                throw BBL::Error{"vector type mismatch: expected int, got " + typeName(val.type())};
            { auto tmp = val.intVal(); memcpy(p, &tmp, sizeof(int64_t)); }
            return;
        case BBL::Type::Float: {
            double v;
            if (val.type() == BBL::Type::Float) v = val.floatVal();
            else if (val.type() == BBL::Type::Int) v = static_cast<double>(val.intVal());
            else throw BBL::Error{"vector type mismatch: expected float, got " + typeName(val.type())};
            memcpy(p, &v, sizeof(double));
            return;
        }
        case BBL::Type::Bool:
            if (val.type() != BBL::Type::Bool)
                throw BBL::Error{"vector type mismatch: expected bool, got " + typeName(val.type())};
            *p = val.boolVal() ? 1 : 0;
            return;
        case BBL::Type::Struct:
            if (val.type() != BBL::Type::Struct || val.structVal()->desc->name != vec->elemType)
                throw BBL::Error{"vector type mismatch: expected struct " + vec->elemType};
            memcpy(p, val.structVal()->data.data(), vec->elemSize);
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
            if (val.type() != BBL::Type::Int) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected int, got " + typeName(val.type())};
            }
            { auto tmp = val.intVal(); memcpy(p, &tmp, sizeof(int64_t)); }
            return;
        }
        case BBL::Type::Float: {
            double v;
            if (val.type() == BBL::Type::Float) {
                v = val.floatVal();
            } else if (val.type() == BBL::Type::Int) {
                v = static_cast<double>(val.intVal());
            } else {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected float, got " + typeName(val.type())};
            }
            memcpy(p, &v, sizeof(double));
            return;
        }
        case BBL::Type::Bool: {
            if (val.type() != BBL::Type::Bool) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected bool, got " + typeName(val.type())};
            }
            *p = val.boolVal() ? 1 : 0;
            return;
        }
        case BBL::Type::Struct: {
            if (val.type() != BBL::Type::Struct || val.structVal()->desc->name != vec->elemType) {
                vec->data.resize(oldSize);
                throw BBL::Error{"vector type mismatch: expected struct " + vec->elemType};
            }
            memcpy(p, val.structVal()->data.data(), vec->elemSize);
            return;
        }
        default:
            vec->data.resize(oldSize);
            throw BBL::Error{"internal: unsupported vector element type"};
    }
}

// ---------- Backtrace ----------

void BblState::printBacktrace(const std::string& what) {
    if (!currentFile.empty() && runtimeLine > 0)
        fprintf(stderr, "%s:%d: %s\n", currentFile.c_str(), runtimeLine, what.c_str());
    else if (runtimeLine > 0)
        fprintf(stderr, "line %d: %s\n", runtimeLine, what.c_str());
    else
        fprintf(stderr, "error: %s\n", what.c_str());
    for (int i = static_cast<int>(callStack.size()) - 1; i >= 0; i--) {
        auto& f = callStack[i];
        fprintf(stderr, "  at %s  %s:%d\n", f.expr.c_str(), f.file.c_str(), f.line);
    }
}

// ---------- addPrint / addStdLib ----------

static std::string valueToString(const BblValue& val) {
    char buf[64];
    switch (val.type()) {
        case BBL::Type::String:
            return val.stringVal()->data;
        case BBL::Type::Int:
            snprintf(buf, sizeof(buf), "%" PRId64, val.intVal());
            return buf;
        case BBL::Type::Float:
            snprintf(buf, sizeof(buf), "%g", val.floatVal());
            return buf;
        case BBL::Type::Bool:
            return val.boolVal() ? "true" : "false";
        case BBL::Type::Null:
            return "null";
        case BBL::Type::Binary:
            snprintf(buf, sizeof(buf), "<binary %zu bytes>", val.binaryVal()->length());
            return buf;
        case BBL::Type::Fn:
            return val.isCFn() ? "<cfn>" : "<fn>";
        case BBL::Type::Table:
            return "<table length=" + std::to_string(val.tableVal()->length()) + ">";
        case BBL::Type::Vector:
            return "<vector " + val.vectorVal()->elemType + " length=" + std::to_string(val.vectorVal()->length()) + ">";
        case BBL::Type::Struct:
            return "<struct " + val.structVal()->desc->name + ">";
        case BBL::Type::UserData:
            return "<userdata " + val.userdataVal()->desc->name + ">";
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
    bbl->pushString(typeName(bbl->getArg(0).type()).c_str());
    return 1;
}

static int bblInt(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"int requires 1 argument"};
    BblValue arg = bbl->getArg(0);
    switch (arg.type()) {
        case BBL::Type::Int:
            bbl->pushInt(arg.intVal());
            return 1;
        case BBL::Type::Float:
            bbl->pushInt(static_cast<int64_t>(arg.floatVal()));
            return 1;
        case BBL::Type::String: {
            const char* str = arg.stringVal()->data.c_str();
            char* end = nullptr;
            errno = 0;
            int64_t val = strtoll(str, &end, 10);
            if (end == str || *end != '\0') {
                throw BBL::Error{"int: cannot parse \"" + arg.stringVal()->data + "\""};
            }
            if (errno == ERANGE) {
                throw BBL::Error{"int: overflow parsing \"" + arg.stringVal()->data + "\""};
            }
            bbl->pushInt(val);
            return 1;
        }
        default:
            throw BBL::Error{"int: cannot convert " + typeName(arg.type()) + " to int"};
    }
}

static int bblFloat(BblState* bbl) {
    if (bbl->argCount() != 1) throw BBL::Error{"float requires 1 argument"};
    BblValue arg = bbl->getArg(0);
    switch (arg.type()) {
        case BBL::Type::Float:
            bbl->pushFloat(arg.floatVal());
            return 1;
        case BBL::Type::Int:
            bbl->pushFloat(static_cast<double>(arg.intVal()));
            return 1;
        case BBL::Type::String: {
            const char* str = arg.stringVal()->data.c_str();
            char* end = nullptr;
            errno = 0;
            double val = strtod(str, &end);
            if (end == str || *end != '\0') {
                throw BBL::Error{"float: cannot parse \"" + arg.stringVal()->data + "\""};
            }
            if (errno == ERANGE) {
                throw BBL::Error{"float: overflow parsing \"" + arg.stringVal()->data + "\""};
            }
            bbl->pushFloat(val);
            return 1;
        }
        default:
            throw BBL::Error{"float: cannot convert " + typeName(arg.type()) + " to float"};
    }
}

static int bblFmt(BblState* bbl) {
    if (bbl->argCount() < 1) throw BBL::Error{"fmt requires at least 1 argument (format string)"};
    BblValue fmtArg = bbl->getArg(0);
    if (fmtArg.type() != BBL::Type::String) throw BBL::Error{"fmt: first argument must be a string"};
    const std::string& fmt = fmtArg.stringVal()->data;

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
    bbl.defn("type-of", bblTypeof);
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
    if (self.type() != BBL::Type::UserData || self.userdataVal()->desc->name != "File") {
        throw BBL::Error{"File.read: expected File object"};
    }
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
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
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.read-bytes: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
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
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.write: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
    if (!fp) throw BBL::Error{"File.write: file is closed"};
    const char* str = bbl->getStringArg(1);
    size_t len = strlen(str);
    size_t written = fwrite(str, 1, len, fp);
    if (written != len) throw BBL::Error{"File.write: write failed"};
    return 0;
}

static int bblFileWriteBytes(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.write-bytes: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
    if (!fp) throw BBL::Error{"File.write-bytes: file is closed"};
    BblBinary* b = bbl->getBinaryArg(1);
    size_t written = fwrite(b->data.data(), 1, b->length(), fp);
    if (written != b->length()) throw BBL::Error{"File.write-bytes: write failed"};
    return 0;
}

static int bblFileReadLine(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.read-line: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
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
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.close: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
    if (fp && fp != stdin && fp != stdout && fp != stderr) {
        fclose(fp);
        self.userdataVal()->data = nullptr;
    }
    return 0;
}

static int bblFileFlush(BblState* bbl) {
    BblValue self = bbl->getArg(0);
    if (self.type() != BBL::Type::UserData) throw BBL::Error{"File.flush: expected File object"};
    FILE* fp = static_cast<FILE*>(self.userdataVal()->data);
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
    bbl.defn("file-bytes", bblFilebytes);
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
    BblTable* tbl = bbl->allocTable();
#ifdef _WIN32
    namespace fs = std::filesystem;
    std::string pattern = bbl->getStringArg(0);
    fs::path dir = fs::path(pattern).parent_path();
    std::string fname = fs::path(pattern).filename().string();
    if (dir.empty()) dir = ".";
    int64_t idx = 1;
    try {
        for (auto& entry : fs::directory_iterator(dir)) {
            std::string name = entry.path().filename().string();
            if (name.find('*') == std::string::npos && fname.find('*') != std::string::npos) {
                tbl->set(BblValue::makeInt(idx++), BblValue::makeString(bbl->intern(entry.path().string())));
            }
        }
    } catch (...) {}
#else
    glob_t g;
    int ret = ::glob(bbl->getStringArg(0), GLOB_NOSORT, nullptr, &g);
    if (ret == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            tbl->set(BblValue::makeInt((int64_t)(i + 1)), BblValue::makeString(bbl->intern(g.gl_pathv[i])));
    }
    if (ret != GLOB_NOMATCH) globfree(&g);
#endif
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
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal()->data);
    if (!proc->pipe) { bbl->pushString(""); return 0; }
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), proc->pipe)) result += buf;
    bbl->pushString(result.c_str());
    return 0;
}

static int bblProcess_readLine(BblState* bbl) {
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal()->data);
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
    auto* proc = static_cast<BblProcess*>(bbl->getArg(0).userdataVal()->data);
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
#ifdef _WIN32
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    char cmdBuf[4096];
    snprintf(cmdBuf, sizeof(cmdBuf), "cmd.exe /c %s", cmd);
    if (!CreateProcessA(nullptr, cmdBuf, nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        throw BBL::Error{"spawn-detached: CreateProcess failed"};
    bbl->pushInt(static_cast<int64_t>(pi.dwProcessId));
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    pid_t pid = fork();
    if (pid < 0) throw BBL::Error{"spawn-detached: fork failed"};
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);
        setsid();
        if (!freopen(devNull(), "r", stdin)) _exit(127);
        if (!freopen(devNull(), "w", stdout)) _exit(127);
        if (!freopen(devNull(), "w", stderr)) _exit(127);
        execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }
    waitpid(pid, nullptr, 0);
    bbl->pushInt(static_cast<int64_t>(pid));
#endif
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

    bbl.defn("get-env", bblOs_getenv);
    bbl.defn("set-env", bblOs_setenv);
    bbl.defn("unset-env", bblOs_unsetenv);
    bbl.defn("clock", bblOs_clock);
    bbl.defn("time", bblOs_time);
    bbl.defn("sleep", bblOs_sleep);
    bbl.defn("exit", bblOs_exit);
    bbl.defn("get-pid", bblOs_getpid);
    bbl.defn("get-cwd", bblOs_getcwd);
    bbl.defn("chdir", bblOs_chdir);
    bbl.defn("mkdir", bblOs_mkdir);
    bbl.defn("remove", bblOs_remove);
    bbl.defn("rename", bblOs_rename);
    bbl.defn("tmp-name", bblOs_tmpname);
    bbl.defn("execute", bblOs_execute);
    bbl.defn("date", bblOs_date);
    bbl.defn("diff-time", bblOs_difftime);
    bbl.defn("stat", bblOs_stat);
    bbl.defn("glob", bblOs_glob);
    bbl.defn("spawn", bblOs_spawn);
    bbl.defn("spawn-detached", bblOs_spawnDetached);

    bbl.defn("exists", [](BblState* b) -> int {
        b->pushBool(std::filesystem::exists(b->getStringArg(0)));
        return 1;
    });
    bbl.defn("path-join", [](BblState* b) -> int {
        namespace fs = std::filesystem;
        fs::path result(b->getStringArg(0));
        for (int i = 1; i < b->argCount(); i++)
            result /= b->getStringArg(i);
        b->pushString(result.string().c_str());
        return 1;
    });
    bbl.defn("path-dir", [](BblState* b) -> int {
        b->pushString(std::filesystem::path(b->getStringArg(0)).parent_path().string().c_str());
        return 1;
    });
    bbl.defn("path-base", [](BblState* b) -> int {
        b->pushString(std::filesystem::path(b->getStringArg(0)).filename().string().c_str());
        return 1;
    });
    bbl.defn("path-ext", [](BblState* b) -> int {
        b->pushString(std::filesystem::path(b->getStringArg(0)).extension().string().c_str());
        return 1;
    });
    bbl.defn("path-abs", [](BblState* b) -> int {
        b->pushString(std::filesystem::absolute(b->getStringArg(0)).string().c_str());
        return 1;
    });
}

std::vector<uint8_t> BBL::lz4Compress(const uint8_t* data, size_t size) {
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    prefs.frameInfo.contentSize = size;
    size_t bound = LZ4F_compressFrameBound(size, &prefs);
    std::vector<uint8_t> out(bound);
    size_t written = LZ4F_compressFrame(out.data(), bound, data, size, &prefs);
    if (LZ4F_isError(written))
        throw BBL::Error{"LZ4 compress failed: " + std::string(LZ4F_getErrorName(written))};
    out.resize(written);
    return out;
}

std::vector<uint8_t> BBL::lz4Decompress(const uint8_t* data, size_t size) {
    LZ4F_dctx* ctx = nullptr;
    size_t ret = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(ret))
        throw BBL::Error{"LZ4 decompress: init failed"};
    LZ4F_frameInfo_t info = {};
    size_t consumed = size;
    ret = LZ4F_getFrameInfo(ctx, &info, data, &consumed);
    if (LZ4F_isError(ret)) {
        LZ4F_freeDecompressionContext(ctx);
        throw BBL::Error{"LZ4 decompress: invalid frame"};
    }
    size_t outSize = info.contentSize ? static_cast<size_t>(info.contentSize) : size * 4;
    if (outSize > 256 * 1024 * 1024) {
        LZ4F_freeDecompressionContext(ctx);
        throw BBL::Error{"LZ4 decompress: output too large"};
    }
    std::vector<uint8_t> out(outSize);
    size_t dstSize = outSize;
    size_t srcSize = size - consumed;
    ret = LZ4F_decompress(ctx, out.data(), &dstSize, data + consumed, &srcSize, nullptr);
    if (LZ4F_isError(ret)) {
        LZ4F_freeDecompressionContext(ctx);
        throw BBL::Error{"LZ4 decompress failed: " + std::string(LZ4F_getErrorName(ret))};
    }
    out.resize(dstSize);
    LZ4F_freeDecompressionContext(ctx);
    return out;
}

// ---------- Networking ----------

struct BblSocket { socket_t fd; bool closed = false; };

static void socketDestructor(void* p) {
    auto* s = static_cast<BblSocket*>(p);
    if (!s->closed) socketClose(s->fd);
    delete s;
}

static void checkSocketOpen(BblSocket* s) {
    if (s->closed) throw BBL::Error{"socket is closed"};
}

static int socketRead(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    std::string result;
    char buf[4096];
    while (true) {
        auto n = recv(s->fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        result.append(buf, static_cast<size_t>(n));
        if (result.size() > 16 * 1024 * 1024) throw BBL::Error{"socket read: exceeded 16MB"};
    }
    bbl->pushString(result.c_str());
    return 1;
}

static int socketReadLine(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    std::string line;
    char c;
    while (line.size() < 65536) {
        auto n = recv(s->fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        line += c;
    }
    bbl->pushString(line.c_str());
    return 1;
}

static int socketReadBytes(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    size_t want = static_cast<size_t>(bbl->getIntArg(1));
    if (want > 16 * 1024 * 1024) throw BBL::Error{"socket read-bytes: max 16MB"};
    std::vector<uint8_t> data(want);
    size_t got = 0;
    while (got < want) {
        auto n = recv(s->fd, reinterpret_cast<char*>(data.data() + got), static_cast<int>(want - got), 0);
        if (n <= 0) throw BBL::Error{"socket read-bytes: connection closed"};
        got += static_cast<size_t>(n);
    }
    bbl->pushBinary(data.data(), data.size());
    return 1;
}

static int socketWrite(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    const char* str = bbl->getStringArg(1);
    size_t len = strlen(str);
    size_t sent = 0;
    while (sent < len) {
        auto n = send(s->fd, str + sent, static_cast<int>(len - sent), MSG_NOSIGNAL);
        if (n <= 0) throw BBL::Error{"socket write failed"};
        sent += static_cast<size_t>(n);
    }
    bbl->pushInt(static_cast<int64_t>(sent));
    return 1;
}

static int socketWriteBytes(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    auto* bin = bbl->getBinaryArg(1);
    size_t len = bin->data.size();
    size_t sent = 0;
    while (sent < len) {
        auto n = send(s->fd, reinterpret_cast<const char*>(bin->data.data() + sent),
                      static_cast<int>(len - sent), MSG_NOSIGNAL);
        if (n <= 0) throw BBL::Error{"socket write-bytes failed"};
        sent += static_cast<size_t>(n);
    }
    bbl->pushInt(static_cast<int64_t>(sent));
    return 1;
}

static int socketCloseMethod(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    if (!s->closed) { socketClose(s->fd); s->closed = true; }
    return 0;
}

static int serverAccept(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    socket_t client = accept(s->fd, nullptr, nullptr);
    if (client == SOCKET_INVALID) throw BBL::Error{"accept failed"};
    auto* cs = new BblSocket{client, false};
    bbl->pushUserData("Socket", static_cast<void*>(cs));
    return 1;
}

static int udpBind(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    const char* addr = bbl->getStringArg(1);
    int port = static_cast<int>(bbl->getIntArg(2));
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(addr, portStr.c_str(), &hints, &res) != 0)
        throw BBL::Error{"udp bind: address resolution failed"};
    int r = ::bind(s->fd, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (r < 0) throw BBL::Error{"udp bind failed"};
    return 0;
}

static int udpSendTo(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    const char* addr = bbl->getStringArg(1);
    int port = static_cast<int>(bbl->getIntArg(2));
    const char* data; size_t len;
    if (bbl->getArgType(3) == BBL::Type::String) {
        data = bbl->getStringArg(3); len = strlen(data);
    } else {
        auto* bin = bbl->getBinaryArg(3);
        data = reinterpret_cast<const char*>(bin->data.data()); len = bin->data.size();
    }
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(addr, portStr.c_str(), &hints, &res) != 0)
        throw BBL::Error{"udp send-to: address resolution failed"};
    auto n = sendto(s->fd, data, static_cast<int>(len), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (n < 0) throw BBL::Error{"udp send-to failed"};
    return 0;
}

static int udpRecvFrom(BblState* bbl) {
    auto* s = static_cast<BblSocket*>(bbl->callArgs[0].userdataVal()->data);
    checkSocketOpen(s);
    size_t maxBytes = static_cast<size_t>(bbl->getIntArg(1));
    if (maxBytes > 65536) maxBytes = 65536;
    std::vector<char> buf(maxBytes);
    struct sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    auto n = recvfrom(s->fd, buf.data(), static_cast<int>(maxBytes), 0,
                      reinterpret_cast<struct sockaddr*>(&from), &fromLen);
    if (n < 0) throw BBL::Error{"udp recv-from failed"};
    BblTable* tbl = bbl->allocTable();
    tbl->set(BblValue::makeString(bbl->intern("data")),
             BblValue::makeString(bbl->intern(std::string(buf.data(), static_cast<size_t>(n)))));
    char addrBuf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, addrBuf, sizeof(addrBuf));
    tbl->set(BblValue::makeString(bbl->intern("addr")), BblValue::makeString(bbl->intern(addrBuf)));
    tbl->set(BblValue::makeString(bbl->intern("port")), BblValue::makeInt(ntohs(from.sin_port)));
    bbl->pushTable(tbl);
    return 1;
}

void BBL::addNet(BblState& bbl) {
    if (bbl.has("tcp-connect")) return;
    socketInit();

    BBL::TypeBuilder sb("Socket");
    sb.method("read", socketRead)
      .method("read-line", socketReadLine)
      .method("read-bytes", socketReadBytes)
      .method("write", socketWrite)
      .method("write-bytes", socketWriteBytes)
      .method("close", socketCloseMethod)
      .destructor(socketDestructor);
    bbl.registerType(sb);

    BBL::TypeBuilder svb("Server");
    svb.method("accept", serverAccept)
       .method("close", socketCloseMethod)
       .destructor(socketDestructor);
    bbl.registerType(svb);

    BBL::TypeBuilder ub("UdpSocket");
    ub.method("bind", udpBind)
      .method("send-to", udpSendTo)
      .method("recv-from", udpRecvFrom)
      .method("close", socketCloseMethod)
      .destructor(socketDestructor);
    bbl.registerType(ub);

    bbl.defn("tcp-connect", [](BblState* b) -> int {
        const char* host = b->getStringArg(0);
        int port = static_cast<int>(b->getIntArg(1));
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        std::string portStr = std::to_string(port);
        int err = getaddrinfo(host, portStr.c_str(), &hints, &res);
        if (err != 0) throw BBL::Error{"tcp-connect: " + std::string(gai_strerror(err))};
        socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd == SOCKET_INVALID) { freeaddrinfo(res); throw BBL::Error{"tcp-connect: socket failed"}; }
        if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) < 0) {
            socketClose(fd); freeaddrinfo(res);
            throw BBL::Error{"tcp-connect: connection refused"};
        }
        freeaddrinfo(res);
        b->pushUserData("Socket", static_cast<void*>(new BblSocket{fd, false}));
        return 1;
    });

    bbl.defn("tcp-listen", [](BblState* b) -> int {
        const char* host = b->getStringArg(0);
        int port = static_cast<int>(b->getIntArg(1));
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        std::string portStr = std::to_string(port);
        if (getaddrinfo(host, portStr.c_str(), &hints, &res) != 0)
            throw BBL::Error{"tcp-listen: address resolution failed"};
        socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd == SOCKET_INVALID) { freeaddrinfo(res); throw BBL::Error{"tcp-listen: socket failed"}; }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        if (::bind(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) < 0) {
            socketClose(fd); freeaddrinfo(res);
            throw BBL::Error{"tcp-listen: bind failed"};
        }
        freeaddrinfo(res);
        if (listen(fd, 16) < 0) { socketClose(fd); throw BBL::Error{"tcp-listen: listen failed"}; }
        b->pushUserData("Server", static_cast<void*>(new BblSocket{fd, false}));
        return 1;
    });

    bbl.defn("udp-open", [](BblState* b) -> int {
        socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == SOCKET_INVALID) throw BBL::Error{"udp-open: socket failed"};
        b->pushUserData("UdpSocket", static_cast<void*>(new BblSocket{fd, false}));
        return 1;
    });
}

void BBL::addCore(BblState& bbl) {
    bbl.defn("compress", [](BblState* b) -> int {
        BblBinary* bin = b->getBinaryArg(0);
        auto out = BBL::lz4Compress(bin->data.data(), bin->data.size());
        b->pushBinary(out.data(), out.size());
        return 1;
    });
    bbl.defn("decompress", [](BblState* b) -> int {
        BblBinary* bin = b->getBinaryArg(0);
        auto out = BBL::lz4Decompress(bin->data.data(), bin->data.size());
        b->pushBinary(out.data(), out.size());
        return 1;
    });
    bbl.defn("exec-binary", [](BblState* b) -> int {
        BblBinary* bin = b->getBinaryArg(0);
        std::string source(reinterpret_cast<const char*>(bin->data.data()), bin->data.size());
        BblValue result = b->execExpr(source);
        b->returnValue = result;
        b->hasReturn = true;
        return 1;
    });

    // --- Random ---
    bbl.defn("random", [](BblState* b) -> int {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        b->pushFloat(dist(b->rng));
        return 1;
    });
    bbl.defn("random-int", [](BblState* b) -> int {
        if (b->argCount() == 1) {
            int64_t max = b->getIntArg(0);
            if (max <= 0) throw BBL::Error{"random-int: max must be positive"};
            std::uniform_int_distribution<int64_t> dist(0, max - 1);
            b->pushInt(dist(b->rng));
        } else {
            int64_t lo = b->getIntArg(0), hi = b->getIntArg(1);
            if (lo > hi) throw BBL::Error{"random-int: min > max"};
            std::uniform_int_distribution<int64_t> dist(lo, hi);
            b->pushInt(dist(b->rng));
        }
        return 1;
    });
    bbl.defn("random-seed", [](BblState* b) -> int {
        b->rng.seed(static_cast<uint64_t>(b->getIntArg(0)));
        return 0;
    });

    // --- Error / Assert ---
    bbl.defn("error", [](BblState* b) -> int {
        std::string msg = b->argCount() > 0 ? b->getStringArg(0) : "error";
        throw BBL::Error{msg};
    });
    bbl.defn("assert", [](BblState* b) -> int {
        BblValue cond = b->getArg(0);
        bool falsy = (cond.type() == BBL::Type::Null) ||
                     (cond.type() == BBL::Type::Bool && !cond.boolVal()) ||
                     (cond.type() == BBL::Type::Int && cond.intVal() == 0);
        if (falsy) {
            std::string msg = b->argCount() > 1 ? b->getStringArg(1) : "assertion failed";
            throw BBL::Error{msg};
        }
        return 0;
    });

    // --- Sort ---
    bbl.defn("sort", [](BblState* b) -> int {
        BblValue arg = b->getArg(0);
        bool hasComp = b->argCount() > 1 && b->getArg(1).type() == BBL::Type::Fn;
        auto defaultLess = [](const BblValue& a, const BblValue& bv) -> bool {
            if (a.type() == bv.type()) {
                if (a.type() == BBL::Type::Int) return a.intVal() < bv.intVal();
                if (a.type() == BBL::Type::Float) return a.floatVal() < bv.floatVal();
                if (a.type() == BBL::Type::String) return a.stringVal()->data < bv.stringVal()->data;
            }
            return static_cast<int>(a.type()) < static_cast<int>(bv.type());
        };
        auto makeCompare = [&](BblClosure* cl) {
            if (!cl->jitCache) cl->jitCache = new JitCode(jitCompile(*b, cl->chunk, cl));
            auto fn = reinterpret_cast<BblValue(*)(BblValue*, BblState*, Chunk*)>(cl->jitCache->buf);
            return [fn, cl, b](const BblValue& a, const BblValue& bv) -> bool {
                BblValue regs[4] = { BblValue::makeClosure(cl), a, bv, BblValue::makeNull() };
                BblValue r = fn(regs, b, &cl->chunk);
                return r.type() == BBL::Type::Bool ? r.boolVal() :
                       r.type() == BBL::Type::Int ? r.intVal() != 0 : false;
            };
        };
        if (arg.type() == BBL::Type::Vector) {
            BblVec* vec = arg.vectorVal();
            size_t n = vec->length();
            std::vector<BblValue> elems(n);
            for (size_t i = 0; i < n; i++) elems[i] = b->readVecElem(vec, i);
            if (hasComp) std::stable_sort(elems.begin(), elems.end(), makeCompare(b->getArg(1).closureVal()));
            else std::stable_sort(elems.begin(), elems.end(), defaultLess);
            for (size_t i = 0; i < n; i++) b->writeVecElem(vec, i, elems[i]);
        } else if (arg.type() == BBL::Type::Table) {
            BblTable* tbl = arg.tableVal();
            int64_t n = tbl->nextIntKey;
            if (static_cast<uint32_t>(n) != tbl->count) throw BBL::Error{"sort: table must have sequential integer keys"};
            std::vector<BblValue> elems(n);
            for (int64_t i = 0; i < n; i++) elems[i] = tbl->get(BblValue::makeInt(i)).value_or(BblValue::makeNull());
            if (hasComp) std::stable_sort(elems.begin(), elems.end(), makeCompare(b->getArg(1).closureVal()));
            else std::stable_sort(elems.begin(), elems.end(), defaultLess);
            for (int64_t i = 0; i < n; i++) tbl->set(BblValue::makeInt(i), elems[i]);
        } else throw BBL::Error{"sort: argument must be vector or table"};
        return 0;
    });

    // --- JSON ---
    bbl.defn("json-parse", [](BblState* b) -> int {
        const char* str = b->getStringArg(0);
        size_t len = strlen(str);
        if (len > 64 * 1024 * 1024) throw BBL::Error{"json-parse: input exceeds 64MB"};
        yyjson_doc* doc = yyjson_read(str, len, 0);
        if (!doc) throw BBL::Error{"json-parse: invalid JSON"};
        std::function<BblValue(yyjson_val*, int)> conv = [&](yyjson_val* v, int d) -> BblValue {
            if (d > 128) throw BBL::Error{"json-parse: nesting too deep"};
            if (yyjson_is_int(v)) return BblValue::makeInt(yyjson_get_sint(v));
            if (yyjson_is_real(v)) return BblValue::makeFloat(yyjson_get_real(v));
            if (yyjson_is_str(v)) return BblValue::makeString(b->intern(yyjson_get_str(v)));
            if (yyjson_is_bool(v)) return BblValue::makeBool(yyjson_get_bool(v));
            if (yyjson_is_null(v)) return BblValue::makeNull();
            if (yyjson_is_arr(v)) {
                BblTable* t = b->allocTable(); int64_t i = 0;
                yyjson_arr_iter it; yyjson_arr_iter_init(v, &it); yyjson_val* e;
                while ((e = yyjson_arr_iter_next(&it))) t->set(BblValue::makeInt(i++), conv(e, d+1));
                return BblValue::makeTable(t);
            }
            if (yyjson_is_obj(v)) {
                BblTable* t = b->allocTable();
                yyjson_obj_iter it; yyjson_obj_iter_init(v, &it); yyjson_val* k;
                while ((k = yyjson_obj_iter_next(&it)))
                    t->set(BblValue::makeString(b->intern(yyjson_get_str(k))), conv(yyjson_obj_iter_get_val(k), d+1));
                return BblValue::makeTable(t);
            }
            return BblValue::makeNull();
        };
        BblValue result = conv(yyjson_doc_get_root(doc), 0);
        yyjson_doc_free(doc);
        b->returnValue = result; b->hasReturn = true;
        return 1;
    });
    bbl.defn("json-encode", [](BblState* b) -> int {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        std::function<yyjson_mut_val*(BblValue, int)> conv = [&](BblValue val, int d) -> yyjson_mut_val* {
            if (d > 128) throw BBL::Error{"json-encode: nesting too deep"};
            switch (val.type()) {
            case BBL::Type::Int: return yyjson_mut_sint(doc, val.intVal());
            case BBL::Type::Float: {
                double f = val.floatVal();
                if (f != f || f == 1.0/0.0 || f == -1.0/0.0) throw BBL::Error{"json-encode: NaN/Infinity"};
                return yyjson_mut_real(doc, f);
            }
            case BBL::Type::String: return yyjson_mut_strcpy(doc, val.stringVal()->data.c_str());
            case BBL::Type::Bool: return yyjson_mut_bool(doc, val.boolVal());
            case BBL::Type::Null: return yyjson_mut_null(doc);
            case BBL::Type::Table: {
                BblTable* tbl = val.tableVal();
                if (tbl->nextIntKey > 0 && static_cast<uint32_t>(tbl->nextIntKey) == tbl->count) {
                    yyjson_mut_val* arr = yyjson_mut_arr(doc);
                    for (int64_t i = 0; i < tbl->nextIntKey; i++)
                        yyjson_mut_arr_add_val(arr, conv(tbl->get(BblValue::makeInt(i)).value_or(BblValue::makeNull()), d+1));
                    return arr;
                }
                yyjson_mut_val* obj = yyjson_mut_obj(doc);
                if (tbl->order) for (auto& k : *tbl->order) {
                    auto v = tbl->get(k).value_or(BblValue::makeNull());
                    std::string ks = k.type() == BBL::Type::String ? k.stringVal()->data : std::to_string(k.intVal());
                    yyjson_mut_val* jk = yyjson_mut_strcpy(doc, ks.c_str());
                    yyjson_mut_val* jv = conv(v, d+1);
                    yyjson_mut_obj_add(obj, jk, jv);
                }
                return obj;
            }
            case BBL::Type::Struct: {
                yyjson_mut_val* obj = yyjson_mut_obj(doc);
                for (auto& fd : val.structVal()->desc->fields) {
                    yyjson_mut_val* jk = yyjson_mut_strcpy(doc, fd.name.c_str());
                    yyjson_mut_val* jv = conv(b->readField(val.structVal(), fd), d+1);
                    yyjson_mut_obj_add(obj, jk, jv);
                }
                return obj;
            }
            case BBL::Type::Vector: {
                yyjson_mut_val* arr = yyjson_mut_arr(doc);
                BblVec* vec = val.vectorVal();
                for (size_t i = 0; i < vec->length(); i++) yyjson_mut_arr_add_val(arr, conv(b->readVecElem(vec, i), d+1));
                return arr;
            }
            default: throw BBL::Error{"json-encode: unsupported type"};
            }
        };
        yyjson_mut_doc_set_root(doc, conv(b->getArg(0), 0));
        size_t len = 0; char* json = yyjson_mut_write(doc, 0, &len);
        if (!json) { yyjson_mut_doc_free(doc); throw BBL::Error{"json-encode: serialization failed"}; }
        b->pushString(json); free(json); yyjson_mut_doc_free(doc);
        return 1;
    });
}

static void addSandbox(BblState& bbl) {
    bbl.defn("sandbox", [](BblState* b) -> int {
        const char* code = b->getStringArg(0);
        BblTable* opts = (b->argCount() > 1 && b->getArg(1).type() == BBL::Type::Table)
            ? b->getArg(1).tableVal() : nullptr;

        BblState child;
        BBL::addMath(child);
        BBL::addCore(child);

        if (opts) {
            auto stepsVal = opts->get(BblValue::makeString(b->intern("steps")));
            if (stepsVal && stepsVal->type() == BBL::Type::Int)
                child.maxSteps = static_cast<size_t>(stepsVal->intVal());

            auto allowVal = opts->get(BblValue::makeString(b->intern("allow")));
            if (allowVal && allowVal->type() == BBL::Type::Table) {
                BblTable* allow = allowVal->tableVal();
                auto has = [&](const char* name) {
                    auto v = allow->get(BblValue::makeString(b->intern(name)));
                    return v && v->type() == BBL::Type::Bool && v->boolVal();
                };
                if (has("print")) BBL::addPrint(child);
                if (has("file"))  BBL::addFileIo(child);
                if (has("os"))    BBL::addOs(child);
                if (has("net"))   BBL::addNet(child);
                if (has("state")) BBL::addChildStates(child);
                if (has("all")) {
                    BBL::addPrint(child);
                    BBL::addFileIo(child);
                    BBL::addOs(child);
                    BBL::addNet(child);
                    BBL::addChildStates(child);
                }
            }
        }

        try {
            BblValue result = child.execExpr(code);
            switch (result.type()) {
            case BBL::Type::Int:
            case BBL::Type::Float:
            case BBL::Type::Bool:
            case BBL::Type::Null:
                b->returnValue = result;
                break;
            case BBL::Type::String:
                b->returnValue = BblValue::makeString(b->intern(result.stringVal()->data));
                break;
            default:
                b->returnValue = BblValue::makeNull();
                break;
            }
        } catch (...) {
            b->returnValue = BblValue::makeNull();
        }
        b->hasReturn = true;
        return 1;
    });
}

void BBL::addStdLib(BblState& bbl) {
    BBL::addPrint(bbl);
    BBL::addMath(bbl);
    BBL::addCore(bbl);
    BBL::addFileIo(bbl);
    BBL::addOs(bbl);
    BBL::addNet(bbl);
    BBL::addChildStates(bbl, false);
    addSandbox(bbl);

    bbl.defn("import", [](BblState* b) -> int {
        namespace fs = std::filesystem;
        const char* pathArg = b->getStringArg(0);
        fs::path base = b->scriptDir.empty() ? fs::current_path() : fs::path(b->scriptDir);
        fs::path resolved = fs::weakly_canonical(base / pathArg);
        std::string key = resolved.string();

        auto cached = b->moduleCache.find(key);
        if (cached != b->moduleCache.end()) {
            b->returnValue = BblValue::makeTable(cached->second);
            b->hasReturn = true;
            return 1;
        }

        if (!fs::exists(resolved))
            throw BBL::Error{"import: file not found: " + key};
        std::ifstream file(resolved, std::ios::binary);
        if (!file.is_open())
            throw BBL::Error{"import: cannot open: " + key};
        std::ostringstream ss;
        ss << file.rdbuf();

        BblTable* env = b->allocTable();

        auto savedEnv = b->currentEnv;
        auto savedFile = b->currentFile;
        auto savedDir = b->scriptDir;
        b->currentEnv = env;
        b->currentFile = resolved.string();
        b->scriptDir = resolved.parent_path().string();

        try {
            b->execExpr(ss.str());
        } catch (...) {
            b->currentEnv = savedEnv;
            b->currentFile = savedFile;
            b->scriptDir = savedDir;
            throw;
        }
        b->currentEnv = savedEnv;
        b->currentFile = savedFile;
        b->scriptDir = savedDir;

        b->moduleCache[key] = env;

        b->returnValue = BblValue::makeTable(env);
        b->hasReturn = true;
        return 1;
    });
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
    if (!table->order) return msg;
    for (auto& k : *table->order) {
        if (k.type() != BBL::Type::String)
            throw BBL::Error{"message key must be a string"};
        auto val = table->get(k).value_or(BblValue::makeNull());
        MessageValue mv;
        mv.type = val.type();
        switch (val.type()) {
            case BBL::Type::Int:    mv.intVal = val.intVal(); break;
            case BBL::Type::Float:  mv.floatVal = val.floatVal(); break;
            case BBL::Type::Bool:   mv.boolVal = val.boolVal(); break;
            case BBL::Type::Null:   break;
            case BBL::Type::String: mv.stringVal = val.stringVal()->data; break;
            default:
                throw BBL::Error{"message value must be int, float, bool, null, or string"};
        }
        msg.entries.emplace_back(k.stringVal()->data, std::move(mv));
    }
    if (vecArg) {
        if (vecArg->type() == BBL::Type::Vector) {
            msg.hasPayload = true;
            msg.payloadData = std::move(vecArg->vectorVal()->data);
            msg.payloadElemType = vecArg->vectorVal()->elemType;
            msg.payloadElemTypeTag = vecArg->vectorVal()->elemTypeTag;
            msg.payloadElemSize = vecArg->vectorVal()->elemSize;
        } else if (vecArg->type() == BBL::Type::Binary) {
            msg.hasPayload = true;
            msg.payloadData = std::move(vecArg->binaryVal()->data);
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
    if (self.type() != BBL::Type::UserData || !self.userdataVal()->data)
        throw BBL::Error{"state handle is invalid (destroyed)"};
    return static_cast<BblStateHandle*>(self.userdataVal()->data);
}

static int statePost(BblState* bbl) {
    auto* handle = getHandle(bbl);
    BblValue tblArg = bbl->getArg(1);
    if (tblArg.type() != BBL::Type::Table)
        throw BBL::Error{"post: first argument must be a table"};
    BblValue* vecArg = nullptr;
    BblValue vecVal;
    if (bbl->argCount() >= 3) {
        vecVal = bbl->getArg(2);
        vecArg = &vecVal;
    }
    BblMessage msg = serializeMessage(bbl, tblArg.tableVal(), vecArg);
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
    if (self.type() != BBL::Type::UserData || !self.userdataVal()->data)
        throw BBL::Error{"state handle is invalid (destroyed)"};
    auto* handle = static_cast<BblStateHandle*>(self.userdataVal()->data);
    stateDestructor(handle);
    self.userdataVal()->data = nullptr;
    return 0;
}

// --- Child-side flat functions ---

static int bblChildPost(BblState* bbl) {
    auto* handle = bbl->handle;
    if (!handle) throw BBL::Error{"post: not inside a child state"};
    BblValue tblArg = bbl->getArg(0);
    if (tblArg.type() != BBL::Type::Table)
        throw BBL::Error{"post: first argument must be a table"};
    BblValue* vecArg = nullptr;
    BblValue vecVal;
    if (bbl->argCount() >= 2) {
        vecVal = bbl->getArg(1);
        vecArg = &vecVal;
    }
    BblMessage msg = serializeMessage(bbl, tblArg.tableVal(), vecArg);
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
