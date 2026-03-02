#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <expected>
#include <string>
#include <string_view>
#include <ostream>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>

struct BblClosure;
struct VmState;

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

enum class GetError { NotFound, TypeMismatch };

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
    bool marked = false;
};

struct BblBinary {
    std::vector<uint8_t> data;
    bool marked = false;
    size_t length() const { return data.size(); }
};

struct BblFn;

// ---------- Struct/Vector support ----------

enum class CType { Int8, Uint8, Int16, Uint16, Int32, Uint32, Int64, Uint64, Float32, Float64, Bool, Struct };

struct FieldDesc {
    std::string name;
    size_t offset;
    size_t size;
    CType ctype;
    std::string structType;
};

struct StructDesc {
    std::string name;
    size_t totalSize;
    std::vector<FieldDesc> fields;
};

struct BblStruct {
    StructDesc* desc;
    std::vector<uint8_t> data;
    bool marked = false;
};

struct BblVec {
    std::string elemType;
    BBL::Type elemTypeTag;
    size_t elemSize;
    std::vector<uint8_t> data;
    bool marked = false;

    size_t length() const { return elemSize > 0 ? data.size() / elemSize : 0; }
    uint8_t* at(size_t i) { return data.data() + i * elemSize; }
    const uint8_t* at(size_t i) const { return data.data() + i * elemSize; }
};

struct BblTable;

typedef void (*BblUserDataDestructor)(void*);

struct UserDataDesc {
    std::string name;
    std::unordered_map<std::string, BblCFunction> methods;
    BblUserDataDestructor destructor = nullptr;
};

struct BblUserData {
    UserDataDesc* desc;
    void* data;
    bool marked = false;
};

namespace BBL {

class StructBuilder {
    std::string name_;
    size_t totalSize_;
    std::vector<FieldDesc> fields_;

    void addField(const std::string& fname, size_t offset, size_t size, CType ct, const std::string& stName = "");

public:
    StructBuilder(const std::string& name, size_t totalSize);
    template<typename T> StructBuilder& field(const std::string& fname, size_t offset);
    StructBuilder& structField(const std::string& fname, size_t offset, const std::string& typeName);

    const std::string& name() const { return name_; }
    size_t totalSize() const { return totalSize_; }
    const std::vector<FieldDesc>& fields() const { return fields_; }
};

template<> inline StructBuilder& StructBuilder::field<float>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(float), CType::Float32); return *this;
}
template<> inline StructBuilder& StructBuilder::field<double>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(double), CType::Float64); return *this;
}
template<> inline StructBuilder& StructBuilder::field<int32_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(int32_t), CType::Int32); return *this;
}
template<> inline StructBuilder& StructBuilder::field<int64_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(int64_t), CType::Int64); return *this;
}
template<> inline StructBuilder& StructBuilder::field<bool>(const std::string& fname, size_t offset) {
    addField(fname, offset, 1, CType::Bool); return *this;
}
template<> inline StructBuilder& StructBuilder::field<int8_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(int8_t), CType::Int8); return *this;
}
template<> inline StructBuilder& StructBuilder::field<uint8_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(uint8_t), CType::Uint8); return *this;
}
template<> inline StructBuilder& StructBuilder::field<int16_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(int16_t), CType::Int16); return *this;
}
template<> inline StructBuilder& StructBuilder::field<uint16_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(uint16_t), CType::Uint16); return *this;
}
template<> inline StructBuilder& StructBuilder::field<uint32_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(uint32_t), CType::Uint32); return *this;
}
template<> inline StructBuilder& StructBuilder::field<uint64_t>(const std::string& fname, size_t offset) {
    addField(fname, offset, sizeof(uint64_t), CType::Uint64); return *this;
}

class TypeBuilder {
    std::string name_;
    std::unordered_map<std::string, BblCFunction> methods_;
    BblUserDataDestructor destructor_ = nullptr;

public:
    explicit TypeBuilder(const std::string& name) : name_(name) {}
    TypeBuilder& method(const std::string& name, BblCFunction fn) { methods_[name] = fn; return *this; }
    TypeBuilder& destructor(BblUserDataDestructor fn) { destructor_ = fn; return *this; }

    const std::string& name() const { return name_; }
    const std::unordered_map<std::string, BblCFunction>& methods() const { return methods_; }
    BblUserDataDestructor getDestructor() const { return destructor_; }
};

} // namespace BBL

struct BblValue {
    BBL::Type type = BBL::Type::Null;
    bool isCFn = false;
    bool isClosure = false;
    union {
        int64_t intVal;
        double floatVal;
        bool boolVal;
        BblString* stringVal;
        BblBinary* binaryVal;
        BblFn* fnVal;
        BblCFunction cfnVal;
        BblStruct* structVal;
        BblVec* vectorVal;
        BblTable* tableVal;
        BblUserData* userdataVal;
        BblClosure* closureVal;
    };

    BblValue() : type(BBL::Type::Null), intVal(0) {}
    static BblValue makeNull() { return {}; }
    static BblValue makeInt(int64_t v) { BblValue r; r.type = BBL::Type::Int; r.intVal = v; return r; }
    static BblValue makeFloat(double v) { BblValue r; r.type = BBL::Type::Float; r.floatVal = v; return r; }
    static BblValue makeBool(bool v) { BblValue r; r.type = BBL::Type::Bool; r.boolVal = v; return r; }
    static BblValue makeString(BblString* s) { BblValue r; r.type = BBL::Type::String; r.stringVal = s; return r; }
    static BblValue makeBinary(BblBinary* b) { BblValue r; r.type = BBL::Type::Binary; r.binaryVal = b; return r; }
    static BblValue makeFn(BblFn* f) { BblValue r; r.type = BBL::Type::Fn; r.fnVal = f; return r; }
    static BblValue makeCFn(BblCFunction f) { BblValue r; r.type = BBL::Type::Fn; r.isCFn = true; r.cfnVal = f; return r; }
    static BblValue makeClosure(BblClosure* c) { BblValue r; r.type = BBL::Type::Fn; r.isClosure = true; r.closureVal = c; return r; }
    static BblValue makeStruct(BblStruct* s) { BblValue r; r.type = BBL::Type::Struct; r.structVal = s; return r; }
    static BblValue makeVector(BblVec* v) { BblValue r; r.type = BBL::Type::Vector; r.vectorVal = v; return r; }
    static BblValue makeTable(BblTable* t) { BblValue r; r.type = BBL::Type::Table; r.tableVal = t; return r; }
    static BblValue makeUserData(BblUserData* u) { BblValue r; r.type = BBL::Type::UserData; r.userdataVal = u; return r; }

    bool operator==(const BblValue& o) const;
    bool operator!=(const BblValue& o) const { return !(*this == o); }
};

struct BblTable {
    struct Entry {
        BblValue key;
        BblValue val;
        bool occupied = false;
        bool tombstone = false;
    };

    Entry* buckets = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    int64_t nextIntKey = 0;
    bool marked = false;
    std::vector<BblValue> order;

    ~BblTable() { delete[] buckets; }
    BblTable() = default;
    BblTable(const BblTable&) = delete;
    BblTable& operator=(const BblTable&) = delete;

    size_t length() const { return count; }
    std::expected<BblValue, BBL::GetError> get(const BblValue& key) const;
    void set(const BblValue& key, const BblValue& val);
    bool has(const BblValue& key) const;
    bool del(const BblValue& key);
};

struct BblScope {
    std::unique_ptr<std::unordered_map<uint32_t, BblValue>> bindings;
    BblScope* parent = nullptr;

    // Flat mode for function call scopes
    std::vector<BblValue> slots;
    const std::unordered_map<uint32_t, size_t>* slotMap = nullptr;

    BblScope() = default;
    explicit BblScope(BblScope* p) : parent(p) {}

    void def(uint32_t id, BblValue val);
    void set(uint32_t id, BblValue val);
    BblValue* lookup(uint32_t id);
};

struct BblFn {
    std::vector<std::string> params;
    std::vector<uint32_t> paramIds;
    std::vector<struct AstNode> body;
    std::vector<std::pair<uint32_t, BblValue>> captures;
    std::unordered_map<uint32_t, size_t> slotIndex;  // symbol ID → slot position
    size_t paramSlotStart = 0;  // first param slot (= captures.size() at build time)
    bool marked = false;
};

// ---------- Lexer ----------

enum class TokenType {
    LParen, RParen,
    Int, Float, String, Symbol,
    Bool, Null, Dot, Colon,
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
        case TokenType::Colon:   return os << "Colon";
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
    ColonAccess,
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
        case NodeType::ColonAccess:   return os << "ColonAccess";
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
    mutable uint32_t symbolId = 0;                // lazy-resolved symbol ID for Symbol nodes
    mutable int8_t cachedSpecialForm = -1;        // lazy-resolved SpecialForm for List head symbols
    mutable bool isTailCall = false;               // marked for tail-call optimization
};

std::vector<AstNode> parse(BblLexer& lexer);

std::string typeName(BBL::Type t);

// ---------- BblState ----------

struct Frame {
    std::string file;
    int line = 0;
    std::string expr;
};

struct BblTerminated {};

// ---------- Child-states messaging ----------

struct MessageValue {
    BBL::Type type = BBL::Type::Null;
    int64_t intVal = 0;
    double floatVal = 0;
    bool boolVal = false;
    std::string stringVal;
};

struct BblMessage {
    std::vector<std::pair<std::string, MessageValue>> entries;
    bool hasPayload = false;
    std::vector<uint8_t> payloadData;
    std::string payloadElemType;
    BBL::Type payloadElemTypeTag = BBL::Type::Null;
    size_t payloadElemSize = 0;
};

struct MessageQueue {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<BblMessage> messages;

    void push(BblMessage msg);
    BblMessage pop(std::atomic<bool>& terminated);
    bool empty();
};

struct BblStateHandle {
    BblState* child = nullptr;
    std::thread thread;
    MessageQueue toChild;
    MessageQueue toParent;
    std::atomic<bool> done{false};
    std::optional<std::string> childError;
};

struct BblState {
    BblScope rootScope;
    std::unordered_map<std::string, BblString*> internTable;
    std::vector<BblString*> allocatedStrings;
    std::vector<BblBinary*> allocatedBinaries;
    std::vector<BblFn*> allocatedFns;
    std::vector<BblStruct*> allocatedStructs;
    std::vector<BblVec*> allocatedVectors;
    std::vector<BblTable*> allocatedTables;
    std::vector<BblUserData*> allocatedUserDatas;
    std::vector<BblClosure*> allocatedClosures;
    std::unique_ptr<VmState> vm;

    // Type descriptors
    std::unordered_map<std::string, StructDesc> structDescs;
    std::unordered_map<std::string, UserDataDesc> userDataDescs;

    // GC
    size_t allocCount = 0;
    size_t gcThreshold = 256;
    size_t savedGcThreshold = 0;
    void pauseGC() { savedGcThreshold = gcThreshold; gcThreshold = SIZE_MAX; }
    void resumeGC() { gcThreshold = savedGcThreshold; }
    std::vector<BblScope*> activeScopes;

    // C function call interface
    std::vector<BblValue> callArgs;
    BblValue returnValue;
    bool hasReturn = false;

    // Backtrace
    std::vector<Frame> callStack;
    size_t maxCallDepth = 512;
    size_t maxSteps = 0;  // 0 = unlimited
    size_t stepCount = 0;
    BblFn* currentFn = nullptr;
    std::string currentFile;
    std::string scriptDir;
    bool allowOpenFilesystem = false;
    bool useBytecode = false;
    bool useJit = false;

    // Flow control (break/continue state flag)
    static constexpr uint8_t FlowNone = 0;
    static constexpr uint8_t FlowBreak = 1;
    static constexpr uint8_t FlowContinue = 2;
    uint8_t flowSignal = FlowNone;

    // Pre-interned method name cache for O(1) dispatch
    struct MethodNames {
        BblString* length = nullptr;
        BblString* push = nullptr;
        BblString* pop = nullptr;
        BblString* clear = nullptr;
        BblString* at = nullptr;
        BblString* set = nullptr;
        BblString* get = nullptr;
        BblString* resize = nullptr;
        BblString* has = nullptr;
        BblString* del = nullptr; // "delete"
        BblString* keys = nullptr;
        BblString* find = nullptr;
        BblString* contains = nullptr;
        BblString* starts_with = nullptr; // "starts-with"
        BblString* ends_with = nullptr;   // "ends-with"
        BblString* slice = nullptr;
        BblString* split = nullptr;
        BblString* replace = nullptr;
        BblString* upper = nullptr;
        BblString* lower = nullptr;
        BblString* trim = nullptr;
        BblString* copy_from = nullptr; // "copy-from"
    } m;

    // Print capture (for testing)
    std::string* printCapture = nullptr;

    // Child-states support
    std::atomic<bool> terminated{false};
    BblStateHandle* handle = nullptr;
    BblValue lastRecvPayload;
    void checkTerminated() { if (terminated.load(std::memory_order_relaxed)) throw BblTerminated{}; }
    void checkStepLimit() {
        if (maxSteps && ++stepCount > maxSteps)
            throw BBL::Error{"step limit exceeded: " + std::to_string(maxSteps) + " steps"};
    }

    // Symbol ID table
    mutable std::unordered_map<std::string, uint32_t> symbolIds;
    mutable uint32_t nextSymbolId = 1;
    uint32_t resolveSymbol(const std::string& name) const;

    BblState();
    ~BblState();

    BblState(const BblState&) = delete;
    BblState& operator=(const BblState&) = delete;

    BblString* intern(const std::string& s);
    BblString* allocString(std::string s);
    BblBinary* allocBinary(std::vector<uint8_t> data);
    BblFn* allocFn();
    BblStruct* allocStruct(StructDesc* desc);
    BblVec* allocVector(const std::string& elemType, BBL::Type elemTypeTag, size_t elemSize);
    BblTable* allocTable();
    BblUserData* allocUserData(const std::string& typeName, void* data);

    void gc();

    void exec(const std::string& source);
    BblValue execExpr(const std::string& source);
    void execfile(const std::string& path);
    std::filesystem::path resolveSandboxPath(const std::string& path, const char* context);
    void defn(const std::string& name, BblCFunction fn);
    void registerStruct(const BBL::StructBuilder& builder);
    void registerType(const BBL::TypeBuilder& builder);

    // Struct/vector helpers
    BblValue readField(BblStruct* s, const FieldDesc& fd);
    void writeField(BblStruct* s, const FieldDesc& fd, const BblValue& val);
    BblValue readVecElem(BblVec* vec, size_t i);
    void writeVecElem(BblVec* vec, size_t i, const BblValue& val);
    void packValue(BblVec* vec, const BblValue& val);
    BblValue constructStruct(StructDesc* desc, const std::vector<BblValue>& args, int callLine);

    template<typename T>
    T* getVectorData(const std::string& name) {
        auto v = get(name);
        if (!v) throw BBL::Error{"undefined symbol: " + name};
        if (v->type != BBL::Type::Vector) {
            throw BBL::Error{"type mismatch: expected vector, got " + std::string(typeName(v->type))};
        }
        return reinterpret_cast<T*>(v->vectorVal->data.data());
    }

    template<typename T>
    size_t getVectorLength(const std::string& name) {
        auto v = get(name);
        if (!v) throw BBL::Error{"undefined symbol: " + name};
        if (v->type != BBL::Type::Vector) {
            throw BBL::Error{"type mismatch: expected vector"};
        }
        return v->vectorVal->length();
    }

    // C function arg access
    int argCount() const;
    bool hasArg(int i) const;
    BBL::Type getArgType(int i) const;
    int64_t getIntArg(int i) const;
    double getFloatArg(int i) const;
    bool getBoolArg(int i) const;
    const char* getStringArg(int i) const;
    BblBinary* getBinaryArg(int i) const;
    BblValue getArg(int i) const;

    // C function return
    void pushInt(int64_t val);
    void pushFloat(double val);
    void pushBool(bool val);
    void pushString(const char* str);
    void pushNull();
    void pushTable(BblTable* tbl);
    void pushBinary(const uint8_t* ptr, size_t size);

    // Introspection
    bool has(const std::string& name) const;
    std::expected<BBL::Type, BBL::GetError> getType(const std::string& name) const;
    std::expected<BblValue, BBL::GetError> get(const std::string& name) const;
    std::expected<int64_t, BBL::GetError> getInt(const std::string& name) const;
    std::expected<double, BBL::GetError> getFloat(const std::string& name) const;
    std::expected<bool, BBL::GetError> getBool(const std::string& name) const;
    std::expected<const char*, BBL::GetError> getString(const std::string& name) const;
    std::expected<BblTable*, BBL::GetError> getTable(const std::string& name) const;
    std::expected<BblBinary*, BBL::GetError> getBinary(const std::string& name) const;

    // Setters
    void setInt(const std::string& name, int64_t val);
    void setFloat(const std::string& name, double val);
    void setString(const std::string& name, const char* str);
    void set(const std::string& name, BblValue val);
    void setBinary(const std::string& name, const uint8_t* ptr, size_t size);
    void pushUserData(const std::string& typeName, void* ptr);

    // Eval
    BblValue eval(const AstNode& node, BblScope& scope);
    BblValue evalList(const AstNode& node, BblScope& scope);
    BblValue callFn(BblFn* fn, const BblValue* args, size_t argc, int callLine);

    // Method dispatch (extracted from evalList)
    BblValue evalBinaryMethod(BblBinary* bin, const std::string& method,
                               const AstNode& node, BblScope& scope);
    BblValue evalVectorMethod(BblVec* vec, const std::string& method,
                              const AstNode& node, BblScope& scope);
    BblValue evalStringMethod(BblString* strObj, const std::string& method,
                              const BblValue& obj, const AstNode& node, BblScope& scope);
    BblValue evalTableMethod(BblTable* tbl, const std::string& method,
                             const AstNode& node, BblScope& scope);

    void printBacktrace(const std::string& what);
};

struct GcPauseGuard {
    BblState* bbl;
    GcPauseGuard(BblState* b) : bbl(b) { bbl->pauseGC(); }
    ~GcPauseGuard() { bbl->resumeGC(); }
};

namespace BBL {
    void addPrint(BblState& bbl);
    void addMath(BblState& bbl);
    void addFileIo(BblState& bbl);
    void addOs(BblState& bbl);
    void addChildStates(BblState& bbl, bool childMode = false);
    void addStdLib(BblState& bbl);
}
