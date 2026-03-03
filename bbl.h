#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
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

enum class ObjKind : uint8_t { Table, Vector, Struct, Binary, UserData };

struct BblBinary {
    ObjKind objKind = ObjKind::Binary;
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
    ObjKind objKind = ObjKind::Struct;
    StructDesc* desc;
    std::vector<uint8_t> data;
    bool marked = false;
};

struct BblVec {
    ObjKind objKind = ObjKind::Vector;
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
    ObjKind objKind = ObjKind::UserData;
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
    uint64_t bits = TAG_NULL;

    static constexpr uint64_t QNAN        = 0xFFF8000000000000ULL;
    static constexpr uint64_t TAG_NULL     = QNAN | (0ULL << 48);
    static constexpr uint64_t TAG_BOOL     = QNAN | (1ULL << 48);
    static constexpr uint64_t TAG_INT      = QNAN | (2ULL << 48);
    static constexpr uint64_t TAG_CLOSURE  = QNAN | (3ULL << 48);
    static constexpr uint64_t TAG_CFN      = QNAN | (4ULL << 48);
    static constexpr uint64_t TAG_FN       = QNAN | (5ULL << 48);
    static constexpr uint64_t TAG_STRING   = QNAN | (6ULL << 48);
    static constexpr uint64_t TAG_OBJECT   = QNAN | (7ULL << 48);
    static constexpr uint64_t TAG_MASK     = QNAN | (7ULL << 48);
    static constexpr uint64_t PAYLOAD_MASK = 0x0000FFFFFFFFFFFFULL;

    bool isDouble()  const { return (bits & QNAN) != QNAN; }

    BBL::Type type() const {
        if (isDouble()) return BBL::Type::Float;
        uint64_t tag = bits & TAG_MASK;
        if (tag == TAG_NULL)    return BBL::Type::Null;
        if (tag == TAG_BOOL)    return BBL::Type::Bool;
        if (tag == TAG_INT)     return BBL::Type::Int;
        if (tag == TAG_CLOSURE || tag == TAG_CFN || tag == TAG_FN) return BBL::Type::Fn;
        if (tag == TAG_STRING)  return BBL::Type::String;
        if (tag == TAG_OBJECT) {
            ObjKind k = *reinterpret_cast<ObjKind*>(bits & PAYLOAD_MASK);
            switch (k) {
                case ObjKind::Table:    return BBL::Type::Table;
                case ObjKind::Vector:   return BBL::Type::Vector;
                case ObjKind::Struct:   return BBL::Type::Struct;
                case ObjKind::Binary:   return BBL::Type::Binary;
                case ObjKind::UserData: return BBL::Type::UserData;
            }
        }
        return BBL::Type::Null;
    }

    template<typename T> T* asPtr() const { return reinterpret_cast<T*>(bits & PAYLOAD_MASK); }

    int64_t  intVal()     const { uint64_t r = bits & PAYLOAD_MASK; return (r & (1ULL<<47)) ? (int64_t)(r | ~PAYLOAD_MASK) : (int64_t)r; }
    double   floatVal()   const { double d; memcpy(&d, &bits, 8); return d; }
    bool     boolVal()    const { return (bits & 1) != 0; }
    BblString*   stringVal()   const { return asPtr<BblString>(); }
    BblBinary*   binaryVal()   const { return asPtr<BblBinary>(); }
    BblFn*       fnVal()       const { return asPtr<BblFn>(); }
    BblCFunction cfnVal()      const { return reinterpret_cast<BblCFunction>(bits & PAYLOAD_MASK); }
    BblClosure*  closureVal()  const { return asPtr<BblClosure>(); }
    BblStruct*   structVal()   const { return asPtr<BblStruct>(); }
    BblVec*      vectorVal()   const { return asPtr<BblVec>(); }
    BblTable*    tableVal()    const { return asPtr<BblTable>(); }
    BblUserData* userdataVal() const { return asPtr<BblUserData>(); }

    bool isClosure() const { return (bits & TAG_MASK) == TAG_CLOSURE; }
    bool isCFn()     const { return (bits & TAG_MASK) == TAG_CFN; }

    static BblValue makeNull()               { BblValue v; v.bits = TAG_NULL; return v; }
    static BblValue makeInt(int64_t n) {
        if (n < -(1LL << 47) || n >= (1LL << 47)) return makeFloat(static_cast<double>(n));
        BblValue v; v.bits = TAG_INT | (static_cast<uint64_t>(n) & PAYLOAD_MASK); return v;
    }
    static BblValue makeFloat(double d)      { BblValue v; memcpy(&v.bits, &d, 8); return v; }
    static BblValue makeBool(bool b)         { BblValue v; v.bits = TAG_BOOL | (b ? 1ULL : 0ULL); return v; }
    static BblValue makeString(BblString* s) { BblValue v; v.bits = TAG_STRING | reinterpret_cast<uint64_t>(s); return v; }
    static BblValue makeBinary(BblBinary* b) { BblValue v; v.bits = TAG_OBJECT | reinterpret_cast<uint64_t>(b); return v; }
    static BblValue makeFn(BblFn* f)         { BblValue v; v.bits = TAG_FN | reinterpret_cast<uint64_t>(f); return v; }
    static BblValue makeCFn(BblCFunction f)  { BblValue v; v.bits = TAG_CFN | reinterpret_cast<uint64_t>(f); return v; }
    static BblValue makeClosure(BblClosure* c){ BblValue v; v.bits = TAG_CLOSURE | reinterpret_cast<uint64_t>(c); return v; }
    static BblValue makeStruct(BblStruct* s) { BblValue v; v.bits = TAG_OBJECT | reinterpret_cast<uint64_t>(s); return v; }
    static BblValue makeVector(BblVec* ve)   { BblValue v; v.bits = TAG_OBJECT | reinterpret_cast<uint64_t>(ve); return v; }
    static BblValue makeTable(BblTable* t)   { BblValue v; v.bits = TAG_OBJECT | reinterpret_cast<uint64_t>(t); return v; }
    static BblValue makeUserData(BblUserData* u) { BblValue v; v.bits = TAG_OBJECT | reinterpret_cast<uint64_t>(u); return v; }

    bool operator==(const BblValue& o) const;
    bool operator!=(const BblValue& o) const { return !(*this == o); }
};

struct BblTable {
    ObjKind objKind = ObjKind::Table;
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

    // Inline storage for small tables (≤2 entries) — avoids hash bucket allocation
    static constexpr size_t INLINE_MAX = 2;
    Entry inlineEntries[INLINE_MAX];
    bool useInline = true;
    bool isSequential = true;
    std::vector<BblValue> arrayPart;

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
    std::vector<BblTable*> freeTablePool;
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

    std::string jitError;
    bool jitHasError = false;

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
    bool useBytecode = true;
    bool useJit = true;

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
        if (v->type() != BBL::Type::Vector) {
            throw BBL::Error{"type mismatch: expected vector, got " + std::string(typeName(v->type()))};
        }
        return reinterpret_cast<T*>(v->vectorVal()->data.data());
    }

    template<typename T>
    size_t getVectorLength(const std::string& name) {
        auto v = get(name);
        if (!v) throw BBL::Error{"undefined symbol: " + name};
        if (v->type() != BBL::Type::Vector) {
            throw BBL::Error{"type mismatch: expected vector"};
        }
        return v->vectorVal()->length();
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
