#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <random>
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

enum class GcType : uint8_t { String, Binary, Struct, Vec, Table, Fn, Closure, UserData };

struct GcObj {
    GcType gcType;
    bool marked = false;
    GcObj* gcNext = nullptr;
};

enum MethodId : uint8_t {
    MID_NONE = 0,
    MID_LENGTH, MID_PUSH, MID_POP, MID_CLEAR, MID_AT, MID_SET, MID_GET,
    MID_RESIZE, MID_RESERVE, MID_HAS, MID_DEL, MID_KEYS, MID_FIND,
    MID_CONTAINS, MID_STARTS_WITH, MID_ENDS_WITH, MID_SLICE, MID_SPLIT,
    MID_REPLACE, MID_UPPER, MID_LOWER, MID_TRIM, MID_COPY_FROM, MID_JOIN,
    MID_TRIM_LEFT, MID_TRIM_RIGHT, MID_PAD_LEFT, MID_PAD_RIGHT,
    MID_AS, MID_SET_AS,
};

struct BblString : GcObj {
    std::string data;
    bool interned = false;
    uint8_t methodId = MID_NONE;
    BblString() { gcType = GcType::String; }
    BblString(const std::string& s) : data(s) { gcType = GcType::String; }
    BblString(const std::string& s, bool m, bool i) : data(s), interned(i) { gcType = GcType::String; marked = m; }
};

enum class ObjKind : uint8_t { Table, Vector, Struct, Binary, UserData };

struct BblBinary : GcObj {
    std::vector<uint8_t> data;
    const char* lazySource = nullptr;
    size_t lazySize = 0;
    bool compressed = false;

    void materialize();
    size_t length() const;
    BblBinary() { gcType = GcType::Binary; }
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

struct BblStruct : GcObj {
    StructDesc* desc;
    std::vector<uint8_t> data;
    BblStruct() { gcType = GcType::Struct; }
};

struct BblVec : GcObj {
    std::string elemType;
    BBL::Type elemTypeTag;
    size_t elemSize;
    std::vector<uint8_t> data;

    size_t length() const { return elemSize > 0 ? data.size() / elemSize : 0; }
    uint8_t* at(size_t i) { return data.data() + i * elemSize; }
    const uint8_t* at(size_t i) const { return data.data() + i * elemSize; }
    BblVec() { gcType = GcType::Vec; }
};

struct BblTable;

typedef void (*BblUserDataDestructor)(void*);

struct UserDataDesc {
    std::string name;
    std::unordered_map<std::string, BblCFunction> methods;
    BblUserDataDestructor destructor = nullptr;
};

struct BblUserData : GcObj {
    UserDataDesc* desc;
    void* data;
    BblUserData() { gcType = GcType::UserData; }
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
            GcType gt = reinterpret_cast<GcObj*>(bits & PAYLOAD_MASK)->gcType;
            switch (gt) {
                case GcType::Table:    return BBL::Type::Table;
                case GcType::Vec:      return BBL::Type::Vector;
                case GcType::Struct:   return BBL::Type::Struct;
                case GcType::Binary:   return BBL::Type::Binary;
                case GcType::UserData: return BBL::Type::UserData;
                default:               return BBL::Type::Null;
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

struct BblTable : GcObj {
    struct Entry {
        BblValue key;
        BblValue val;
        bool isEmpty() const { return key.bits == EMPTY_KEY; }
        bool isTombstone() const { return key.bits == TOMBSTONE_KEY; }
        bool isOccupied() const { return !isEmpty() && !isTombstone(); }
    };
    static constexpr uint64_t EMPTY_KEY = 0;
    static constexpr uint64_t TOMBSTONE_KEY = 1;

    Entry* buckets = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;
    int64_t nextIntKey = 0;
    std::vector<BblValue>* order = nullptr;
    Entry inlineBuckets[4];

    ~BblTable() { if (buckets != inlineBuckets) free(buckets); delete order; }
    BblTable() {
        gcType = GcType::Table;
        buckets = inlineBuckets;
        capacity = 4;
        memset(inlineBuckets, 0, sizeof(inlineBuckets));
    }
    BblTable(const BblTable&) = delete;
    BblTable& operator=(const BblTable&) = delete;

    size_t length() const { return count; }
    std::expected<BblValue, BBL::GetError> get(const BblValue& key) const;
    void set(const BblValue& key, const BblValue& val);
    bool has(const BblValue& key) const;
    bool del(const BblValue& key);
};

struct BblFn : GcObj {
    std::vector<std::string> params;
    std::vector<uint32_t> paramIds;
    std::vector<struct AstNode> body;
    std::vector<std::pair<uint32_t, BblValue>> captures;
    std::unordered_map<uint32_t, size_t> slotIndex;
    size_t paramSlotStart = 0;
    BblFn() { gcType = GcType::Fn; }
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
    const char* binarySource = nullptr;
    size_t binarySize = 0;
    bool isCompressed = false;
    int line = 1;
    int sourceStart = 0;
    int sourceEnd = 0;
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
    Token readCompressedBinary();

public:
    explicit BblLexer(const char* source);
    BblLexer(const char* source, size_t length);
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
    const char* binarySource = nullptr;
    size_t binarySize = 0;
    bool isCompressed = false;
    std::vector<AstNode> children;
    int line = 1;
    mutable uint32_t symbolId = 0;                // lazy-resolved symbol ID for Symbol nodes

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

template<typename T, size_t SLAB_COUNT = 256>
struct SlabAllocator {
    struct Slab {
        alignas(T) char storage[sizeof(T) * SLAB_COUNT];
        size_t used = 0;
    };
    std::vector<std::unique_ptr<Slab>> slabs;
    std::vector<T*> freeList;

    T* alloc() {
        if (!freeList.empty()) {
            T* p = freeList.back();
            freeList.pop_back();
            return new(p) T();
        }
        if (slabs.empty() || slabs.back()->used >= SLAB_COUNT)
            slabs.push_back(std::make_unique<Slab>());
        auto& slab = *slabs.back();
        T* p = reinterpret_cast<T*>(slab.storage + sizeof(T) * slab.used);
        slab.used++;
        return new(p) T();
    }

    void free(T* p) {
        p->~T();
        freeList.push_back(p);
    }

    ~SlabAllocator() {
        for (auto& slab : slabs) {
            for (size_t i = 0; i < slab->used; i++) {
                auto* p = reinterpret_cast<T*>(slab->storage + sizeof(T) * i);
                (void)p;
            }
        }
    }
};

struct BblState {
    std::unordered_map<std::string, BblString*> internTable;
    GcObj* gcHead = nullptr;
    SlabAllocator<BblTable> tableSlab;
    std::unique_ptr<VmState> vm;

    // Type descriptors
    std::unordered_map<std::string, StructDesc> structDescs;
    std::unordered_map<std::string, UserDataDesc> userDataDescs;

    // GC
    size_t allocCount = 0;
    size_t gcThreshold = 4096;
    size_t savedGcThreshold = 0;
    void pauseGC() { savedGcThreshold = gcThreshold; gcThreshold = SIZE_MAX; }
    void resumeGC() { gcThreshold = savedGcThreshold; }

    struct SliceCacheEntry { BblString* src = nullptr; uint32_t pos = 0; uint32_t len = 0; BblString* result = nullptr; };
    static constexpr int SLICE_CACHE_SIZE = 32;
    SliceCacheEntry sliceCache[SLICE_CACHE_SIZE] = {};

    std::string jitError;
    bool jitHasError = false;

    // C function call interface
    std::vector<BblValue> callArgs;
    BblValue returnValue;
    bool hasReturn = false;

    // Backtrace
    std::vector<Frame> callStack;
    size_t maxSteps = 0;  // 0 = unlimited
    size_t stepCount = 0;
    BblFn* currentFn = nullptr;
    std::string currentFile;
    int runtimeLine = 0;
    std::mt19937_64 rng{std::random_device{}()};
    std::string scriptDir;
    bool allowOpenFilesystem = false;
    size_t execDepth = 0;
    static constexpr size_t MAX_EXEC_DEPTH = 64;

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
        BblString* reserve = nullptr;
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
        BblString* join = nullptr;
        BblString* trim_left = nullptr;
        BblString* trim_right = nullptr;
        BblString* pad_left = nullptr;
        BblString* pad_right = nullptr;
        BblString* as = nullptr;
        BblString* set_as = nullptr;
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
    BblBinary* allocLazyBinary(const char* src, size_t size, bool compressed = false);
    BblFn* allocFn();
    BblStruct* allocStruct(StructDesc* desc);
    BblVec* allocVector(const std::string& elemType, BBL::Type elemTypeTag, size_t elemSize);
    BblTable* allocTable();
    BblUserData* allocUserData(const std::string& typeName, void* data);

    void gc();

    void exec(const std::string& source);
    void materializeLazyBinaries();
    BblValue execExpr(const std::string& source);
    void execfile(const std::string& path);
    BblValue execfileExpr(const std::string& path);
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
    void addNet(BblState& bbl);
    void addCore(BblState& bbl);
    void addStdLib(BblState& bbl);

    std::vector<uint8_t> lz4Compress(const uint8_t* data, size_t size);
    std::vector<uint8_t> lz4Decompress(const uint8_t* data, size_t size);
}
