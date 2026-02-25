#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <ostream>
#include <unordered_map>
#include <vector>
#include <functional>

namespace BBL {

enum class Type {
    Null,
    Bool,
    Int,
    Float,
    String,
    Binary,
    Fn,
    Vector,
    Table,
    Struct,
    UserData,
};

struct Error {
    std::string what;
};

} // namespace BBL

inline std::ostream& operator<<(std::ostream& os, BBL::Type t) {
    switch (t) {
        case BBL::Type::Null:     return os << "Null";
        case BBL::Type::Bool:     return os << "Bool";
        case BBL::Type::Int:      return os << "Int";
        case BBL::Type::Float:    return os << "Float";
        case BBL::Type::String:   return os << "String";
        case BBL::Type::Binary:   return os << "Binary";
        case BBL::Type::Fn:       return os << "Fn";
        case BBL::Type::Vector:   return os << "Vector";
        case BBL::Type::Table:    return os << "Table";
        case BBL::Type::Struct:   return os << "Struct";
        case BBL::Type::UserData: return os << "UserData";
    }
    return os << "Unknown";
}

struct BblState;

typedef int (*BblCFunction)(BblState* bbl);

struct BblString {
    std::string data;
};

struct BblBinary {
    std::vector<uint8_t> data;
    size_t length() const { return data.size(); }
};

struct BblFn;

struct BblValue {
    BBL::Type type = BBL::Type::Null;
    union {
        int64_t intVal;
        double floatVal;
        bool boolVal;
        BblString* stringVal;
        BblBinary* binaryVal;
        BblFn* fnVal;
    };

    BblValue() : type(BBL::Type::Null), intVal(0) {}
    static BblValue makeNull() { return {}; }
    static BblValue makeInt(int64_t v) { BblValue r; r.type = BBL::Type::Int; r.intVal = v; return r; }
    static BblValue makeFloat(double v) { BblValue r; r.type = BBL::Type::Float; r.floatVal = v; return r; }
    static BblValue makeBool(bool v) { BblValue r; r.type = BBL::Type::Bool; r.boolVal = v; return r; }
    static BblValue makeString(BblString* s) { BblValue r; r.type = BBL::Type::String; r.stringVal = s; return r; }
    static BblValue makeBinary(BblBinary* b) { BblValue r; r.type = BBL::Type::Binary; r.binaryVal = b; return r; }
    static BblValue makeFn(BblFn* f) { BblValue r; r.type = BBL::Type::Fn; r.fnVal = f; return r; }

    bool operator==(const BblValue& o) const;
    bool operator!=(const BblValue& o) const { return !(*this == o); }
};

struct BblScope {
    std::unordered_map<std::string, BblValue> bindings;
    BblScope* parent = nullptr;

    BblScope() = default;
    explicit BblScope(BblScope* p) : parent(p) {}

    void def(const std::string& name, BblValue val);
    void set(const std::string& name, BblValue val);
    BblValue* lookup(const std::string& name);
};

struct BblFn {
    std::vector<std::string> params;
    std::vector<struct AstNode> body;
    std::vector<std::pair<std::string, BblValue>> captures;
};

// ---------- Lexer ----------

enum class TokenType {
    LParen, RParen,
    Int, Float, String, Symbol,
    Bool, Null, Dot,
    Binary,
    Eof,
};

inline std::ostream& operator<<(std::ostream& os, TokenType t) {
    switch (t) {
        case TokenType::LParen:  return os << "LParen";
        case TokenType::RParen:  return os << "RParen";
        case TokenType::Int:     return os << "Int";
        case TokenType::Float:   return os << "Float";
        case TokenType::String:  return os << "String";
        case TokenType::Symbol:  return os << "Symbol";
        case TokenType::Bool:    return os << "Bool";
        case TokenType::Null:    return os << "Null";
        case TokenType::Dot:     return os << "Dot";
        case TokenType::Binary:  return os << "Binary";
        case TokenType::Eof:     return os << "Eof";
    }
    return os << "Unknown";
}

struct Token {
    TokenType type;
    int64_t intVal = 0;
    double floatVal = 0;
    bool boolVal = false;
    std::string stringVal;
    std::vector<uint8_t> binaryData;
    int line = 1;
};

class BblLexer {
    const char* src;
    int pos = 0;
    int len = 0;
    int line = 1;

    char peek() const;
    char advance();
    void skipWhitespaceAndComments();
    Token readString();
    Token readNumber();
    Token readSymbolOrKeyword();
    Token readBinary();

public:
    explicit BblLexer(const char* source);
    Token nextToken();
    int currentLine() const { return line; }
};

// ---------- AST ----------

enum class NodeType {
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    BoolLiteral,
    NullLiteral,
    BinaryLiteral,
    Symbol,
    List,
    DotAccess,
};

inline std::ostream& operator<<(std::ostream& os, NodeType t) {
    switch (t) {
        case NodeType::IntLiteral:    return os << "IntLiteral";
        case NodeType::FloatLiteral:  return os << "FloatLiteral";
        case NodeType::StringLiteral: return os << "StringLiteral";
        case NodeType::BoolLiteral:   return os << "BoolLiteral";
        case NodeType::NullLiteral:   return os << "NullLiteral";
        case NodeType::BinaryLiteral: return os << "BinaryLiteral";
        case NodeType::Symbol:        return os << "Symbol";
        case NodeType::List:          return os << "List";
        case NodeType::DotAccess:     return os << "DotAccess";
    }
    return os << "Unknown";
}

struct AstNode {
    NodeType type;
    int64_t intVal = 0;
    double floatVal = 0;
    bool boolVal = false;
    std::string stringVal;
    std::vector<uint8_t> binaryData;
    std::vector<AstNode> children;
    int line = 1;
};

std::vector<AstNode> parse(BblLexer& lexer);

// ---------- BblState ----------

struct BblState {
    BblScope rootScope;
    std::unordered_map<std::string, BblString*> internTable;
    std::vector<BblString*> allocatedStrings;
    std::vector<BblBinary*> allocatedBinaries;
    std::vector<BblFn*> allocatedFns;

    BblState();
    ~BblState();

    BblState(const BblState&) = delete;
    BblState& operator=(const BblState&) = delete;

    BblString* intern(const std::string& s);
    BblBinary* allocBinary(std::vector<uint8_t> data);
    BblFn* allocFn();

    void exec(const std::string& source);
    void execfile(const std::string& path);

    // Introspection
    bool has(const std::string& name) const;
    BBL::Type getType(const std::string& name) const;
    BblValue get(const std::string& name) const;
    int64_t getInt(const std::string& name) const;
    double getFloat(const std::string& name) const;
    bool getBool(const std::string& name) const;
    const char* getString(const std::string& name) const;

    // Eval
    BblValue eval(const AstNode& node, BblScope& scope);
    BblValue evalList(const AstNode& node, BblScope& scope);
    BblValue callFn(BblFn* fn, const std::vector<BblValue>& args, int callLine);
};
