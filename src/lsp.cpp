#include "lsp.h"
#include "bbl.h"
#include "vm.h"
#include <yyjson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <filesystem>

static std::string readMessage() {
    char buf[256];
    int contentLength = -1;
    while (fgets(buf, sizeof(buf), stdin)) {
        if (strncmp(buf, "Content-Length:", 15) == 0) {
            contentLength = atoi(buf + 15);
        }
        if (strcmp(buf, "\r\n") == 0 || strcmp(buf, "\n") == 0) break;
    }
    if (contentLength < 0) return "";
    std::string body(contentLength, '\0');
    size_t read = fread(body.data(), 1, contentLength, stdin);
    body.resize(read);
    return body;
}

static void sendMessage(const std::string& json) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", json.size(), json.c_str());
    fflush(stdout);
}

static std::string jsonStr(yyjson_val* val) {
    const char* s = yyjson_get_str(val);
    return s ? s : "";
}

static std::string serializeDoc(yyjson_mut_doc* doc) {
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    std::string result(json, len);
    free(json);
    return result;
}

static std::string makeResponse(int id, yyjson_mut_doc* doc, yyjson_mut_val* result) {
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", id);
    yyjson_mut_obj_add_val(doc, root, "result", result);
    yyjson_mut_doc_set_root(doc, root);
    return serializeDoc(doc);
}

static int lspNoOp(BblState*) { return 0; }
static int lspReturnNull(BblState* b) { b->pushNull(); return 1; }
static int lspReturnZero(BblState* b) { b->pushFloat(0); return 1; }
static int lspReturnEmptyStr(BblState* b) { b->pushString(""); return 1; }

static void addLspStdLib(BblState& bbl) {
    BBL::addMath(bbl);
    BBL::addPrint(bbl);
    bbl.defn("print", lspNoOp);

    bbl.defn("fopen", lspReturnNull);
    bbl.defn("filebytes", lspReturnNull);
    bbl.defn("getenv", lspReturnNull);
    bbl.defn("setenv", lspNoOp);
    bbl.defn("unsetenv", lspNoOp);
    bbl.defn("clock", lspReturnZero);
    bbl.defn("time", lspReturnZero);
    bbl.defn("sleep", lspNoOp);
    bbl.defn("exit", lspNoOp);
    bbl.defn("execute", lspNoOp);
    bbl.defn("spawn", lspReturnNull);
    bbl.defn("spawn-detached", lspReturnNull);
    bbl.defn("getpid", lspReturnZero);
    bbl.defn("getcwd", lspReturnEmptyStr);
    bbl.defn("chdir", lspNoOp);
    bbl.defn("mkdir", lspNoOp);
    bbl.defn("remove", lspNoOp);
    bbl.defn("rename", lspNoOp);
    bbl.defn("tmpname", lspReturnEmptyStr);
    bbl.defn("date", lspReturnEmptyStr);
    bbl.defn("difftime", lspReturnZero);
    bbl.defn("stat", lspReturnNull);
    bbl.defn("glob", [](BblState* b) -> int { b->pushTable(b->allocTable()); return 1; });
    bbl.defn("state-new", lspReturnNull);
    bbl.defn("compress", lspReturnNull);
    bbl.defn("decompress", lspReturnNull);
}

static std::unique_ptr<BblState> analyzeDocument(const std::string& text, const std::string& uri) {
    auto state = std::make_unique<BblState>();
    addLspStdLib(*state);
    state->allowOpenFilesystem = true;
    state->maxSteps = 1000000;
    namespace fs = std::filesystem;
    if (uri.starts_with("file://")) {
        std::string path = uri.substr(7);
        state->scriptDir = fs::path(path).parent_path().string();
    }
    try {
        state->exec(text);
    } catch (...) {}
    return state;
}

struct LspDoc {
    std::string uri;
    std::string text;
    std::unique_ptr<BblState> analysis;
};

static std::unordered_map<std::string, LspDoc> documents;

static std::string publishDiagnostics(const std::string& uri, const std::string& text) {
    std::string errMsg;
    int errLine = 0;
    try {
        BblLexer lexer(text.c_str(), text.size());
        parse(lexer);
    } catch (const BBL::Error& e) {
        errMsg = e.what;
        if (auto pos = errMsg.find("line "); pos != std::string::npos)
            errLine = std::max(0, atoi(errMsg.c_str() + pos + 5) - 1);
    }

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, root, "method", "textDocument/publishDiagnostics");
    yyjson_mut_val* params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, params, "uri", uri.c_str());
    yyjson_mut_val* diags = yyjson_mut_arr(doc);

    if (!errMsg.empty()) {
        yyjson_mut_val* diag = yyjson_mut_obj(doc);
        yyjson_mut_val* range = yyjson_mut_obj(doc);
        yyjson_mut_val* start = yyjson_mut_obj(doc);
        yyjson_mut_val* end = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, start, "line", errLine);
        yyjson_mut_obj_add_int(doc, start, "character", 0);
        yyjson_mut_obj_add_int(doc, end, "line", errLine);
        yyjson_mut_obj_add_int(doc, end, "character", 100);
        yyjson_mut_obj_add_val(doc, range, "start", start);
        yyjson_mut_obj_add_val(doc, range, "end", end);
        yyjson_mut_obj_add_val(doc, diag, "range", range);
        yyjson_mut_obj_add_int(doc, diag, "severity", 1);
        yyjson_mut_obj_add_strcpy(doc, diag, "message", errMsg.c_str());
        yyjson_mut_arr_add_val(diags, diag);
    }

    yyjson_mut_obj_add_val(doc, params, "diagnostics", diags);
    yyjson_mut_obj_add_val(doc, root, "params", params);
    yyjson_mut_doc_set_root(doc, root);
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    std::string result(json ? json : "", json ? len : 0);
    free(json);
    yyjson_mut_doc_free(doc);
    return result;
}

static const char* BUILTIN_FUNCS[] = {
    "print", "str", "typeof", "int", "float", "fmt",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "sqrt", "abs", "floor", "ceil", "min", "max", "pow",
    "log", "log2", "log10", "exp",
    "filebytes", "fopen",
    "getenv", "setenv", "clock", "time", "sleep", "exit",
    "getcwd", "chdir", "mkdir", "remove", "rename", "execute",
    "glob", "spawn", "compress", "decompress",
    nullptr
};

static const char* KEYWORDS[] = {
    "if", "loop", "each", "fn", "do", "with", "try", "catch",
    "break", "continue", "and", "or", "not",
    "vector", "table", "struct", "binary", "int", "sizeof",
    "exec", "execfile", "=",
    "shl", "shr", "band", "bor", "bxor", "bnot",
    nullptr
};

static const char* METHODS[] = {
    "length", "push", "pop", "clear", "at", "set", "get",
    "resize", "reserve", "has", "delete", "keys", "find",
    "contains", "starts-with", "ends-with", "slice", "split",
    "replace", "upper", "lower", "trim", "trim-left", "trim-right",
    "pad-left", "pad-right", "copy-from", "join",
    "read", "read-line", "read-bytes", "write", "write-bytes",
    "close", "flush", "post", "recv", "wait",
    nullptr
};

static std::string extractVarBefore(const std::string& text, int line, int ch, char& trigger) {
    int curLine = 0, idx = 0;
    for (; idx < (int)text.size() && curLine < line; idx++)
        if (text[idx] == '\n') curLine++;
    idx += ch - 1;
    if (idx < 0 || idx >= (int)text.size()) return "";
    auto isSym = [](char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'; };
    while (idx >= 0 && isSym(text[idx])) idx--;
    if (idx < 0) return "";
    trigger = text[idx];
    if (trigger != ':' && trigger != '.') return "";
    int end = idx;
    idx--;
    while (idx >= 0 && isSym(text[idx])) idx--;
    return text.substr(idx + 1, end - idx - 1);
}

static void addMethodCompletions(yyjson_mut_doc* doc, yyjson_mut_val* items, const char** methods) {
    for (int i = 0; methods[i]; i++) {
        yyjson_mut_val* item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "label", methods[i]);
        yyjson_mut_obj_add_int(doc, item, "kind", 2);
        yyjson_mut_arr_add_val(items, item);
    }
}

static const char* TABLE_METHODS[] = { "length", "push", "pop", "get", "set", "has", "delete", "keys", "at", "clear", nullptr };
static const char* STRING_METHODS[] = { "length", "at", "find", "slice", "split", "replace", "contains", "starts-with", "ends-with", "upper", "lower", "trim", "trim-left", "trim-right", "pad-left", "pad-right", "join", nullptr };
static const char* VECTOR_METHODS[] = { "length", "push", "pop", "at", "set", "resize", "reserve", "clear", nullptr };
static const char* BINARY_METHODS[] = { "length", "at", "set", "slice", "resize", "copy-from", nullptr };

static std::string handleCompletion(int id, yyjson_val* params) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* items = yyjson_mut_arr(doc);

    yyjson_val* pos = yyjson_obj_get(params, "position");
    yyjson_val* td = yyjson_obj_get(params, "textDocument");
    std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
    int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
    int ch = yyjson_get_int(yyjson_obj_get(pos, "character"));

    auto dit = documents.find(uri);
    char trigger = 0;
    std::string varName = (dit != documents.end()) ? extractVarBefore(dit->second.text, line, ch, trigger) : "";

    if (!varName.empty() && (trigger == ':' || trigger == '.')) {
        bool specialized = false;
        if (dit != documents.end() && dit->second.analysis) {
            auto val = dit->second.analysis->get(varName);
            if (val) {
                specialized = true;
                switch (val->type()) {
                case BBL::Type::Table: {
                    BblTable* tbl = val->tableVal();
                    if (trigger == '.') {
                        if (tbl->order) {
                            for (auto& k : *tbl->order) {
                                if (k.type() == BBL::Type::String) {
                                    yyjson_mut_val* item = yyjson_mut_obj(doc);
                                    yyjson_mut_obj_add_strcpy(doc, item, "label", k.stringVal()->data.c_str());
                                    yyjson_mut_obj_add_int(doc, item, "kind", 10);
                                    yyjson_mut_arr_add_val(items, item);
                                }
                            }
                        }
                    } else {
                        if (tbl->order) {
                            for (auto& k : *tbl->order) {
                                if (k.type() == BBL::Type::String) {
                                    auto v = tbl->get(k);
                                    if (v && v->type() == BBL::Type::Fn) {
                                        yyjson_mut_val* item = yyjson_mut_obj(doc);
                                        yyjson_mut_obj_add_strcpy(doc, item, "label", k.stringVal()->data.c_str());
                                        yyjson_mut_obj_add_int(doc, item, "kind", 2);
                                        yyjson_mut_arr_add_val(items, item);
                                    }
                                }
                            }
                        }
                        addMethodCompletions(doc, items, TABLE_METHODS);
                    }
                    break;
                }
                case BBL::Type::String: addMethodCompletions(doc, items, STRING_METHODS); break;
                case BBL::Type::Vector: addMethodCompletions(doc, items, VECTOR_METHODS); break;
                case BBL::Type::Binary: addMethodCompletions(doc, items, BINARY_METHODS); break;
                case BBL::Type::Struct: {
                    BblStruct* s = val->structVal();
                    if (s->desc) {
                        for (auto& f : s->desc->fields) {
                            yyjson_mut_val* item = yyjson_mut_obj(doc);
                            yyjson_mut_obj_add_strcpy(doc, item, "label", f.name.c_str());
                            yyjson_mut_obj_add_int(doc, item, "kind", 10);
                            yyjson_mut_arr_add_val(items, item);
                        }
                    }
                    break;
                }
                default: specialized = false; break;
                }
            }
        }
        if (!specialized) addMethodCompletions(doc, items, METHODS);
    } else {
        for (int i = 0; KEYWORDS[i]; i++) {
            yyjson_mut_val* item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "label", KEYWORDS[i]);
            yyjson_mut_obj_add_int(doc, item, "kind", 14);
            yyjson_mut_arr_add_val(items, item);
        }
        for (int i = 0; BUILTIN_FUNCS[i]; i++) {
            yyjson_mut_val* item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "label", BUILTIN_FUNCS[i]);
            yyjson_mut_obj_add_int(doc, item, "kind", 3);
            yyjson_mut_arr_add_val(items, item);
        }
        if (dit != documents.end() && dit->second.analysis && dit->second.analysis->vm) {
            for (auto& [symId, val] : dit->second.analysis->vm->globals) {
                if (val.type() == BBL::Type::Fn && val.isClosure()) {
                    for (auto& [name, id] : dit->second.analysis->symbolIds) {
                        if (id == symId && name[0] != '_') {
                            yyjson_mut_val* item = yyjson_mut_obj(doc);
                            yyjson_mut_obj_add_strcpy(doc, item, "label", name.c_str());
                            yyjson_mut_obj_add_int(doc, item, "kind", 3);
                            std::string detail = "fn(" + std::to_string(val.closureVal()->arity) + " args)";
                            yyjson_mut_obj_add_strcpy(doc, item, "detail", detail.c_str());
                            yyjson_mut_arr_add_val(items, item);
                            break;
                        }
                    }
                }
            }
        }
    }

    std::string result = makeResponse(id, doc, items);
    yyjson_mut_doc_free(doc);
    return result;
}

static std::string handleHover(int id, yyjson_val* params) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);

    yyjson_val* pos = yyjson_obj_get(params, "position");
    yyjson_val* td = yyjson_obj_get(params, "textDocument");
    std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
    int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
    int ch = yyjson_get_int(yyjson_obj_get(pos, "character"));

    std::string word;
    auto dit = documents.find(uri);
    if (dit != documents.end()) {
        const std::string& text = dit->second.text;
        int curLine = 0, idx = 0;
        for (; idx < (int)text.size() && curLine < line; idx++)
            if (text[idx] == '\n') curLine++;
        idx += ch;
        int start = idx, end = idx;
        auto isSym = [](char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'; };
        while (start > 0 && isSym(text[start-1])) start--;
        while (end < (int)text.size() && isSym(text[end])) end++;
        word = text.substr(start, end - start);
    }

    static const std::unordered_map<std::string, std::string> docs = {
        {"print", "print(args...) — output values to stdout"},
        {"str", "str(val) — convert value to string"},
        {"typeof", "typeof(val) — return type name as string"},
        {"int", "int(val) — convert to integer"},
        {"float", "float(val) — convert to float"},
        {"if", "(if cond then else?) — conditional"},
        {"loop", "(loop cond body...) — while loop"},
        {"each", "(each var collection body...) — iterate"},
        {"fn", "(fn (params...) body...) — function"},
        {"do", "(do body...) — sequential block"},
        {"with", "(with var init body...) — RAII resource management"},
        {"try", "(try body... (catch var handler...)) — error handling"},
        {"vector", "(vector type vals...) — typed array"},
        {"table", "(table key val ...) — hash map"},
        {"struct", "(struct Name type field ...) — C-compatible struct"},
        {"binary", "(binary size-or-source) — byte buffer"},
        {"compress", "(compress binary) — LZ4 compress"},
        {"decompress", "(decompress binary) — LZ4 decompress"},
    };

    yyjson_mut_val* result;
    auto it = docs.find(word);
    if (it != docs.end()) {
        result = yyjson_mut_obj(doc);
        yyjson_mut_val* contents = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, contents, "kind", "markdown");
        yyjson_mut_obj_add_str(doc, contents, "value", it->second.c_str());
        yyjson_mut_obj_add_val(doc, result, "contents", contents);
    } else if (dit != documents.end() && dit->second.analysis && !word.empty()) {
        auto val = dit->second.analysis->get(word);
        if (val) {
            std::string info;
            switch (val->type()) {
            case BBL::Type::Int: info = "int = " + std::to_string(val->intVal()); break;
            case BBL::Type::Float: info = "float = " + std::to_string(val->floatVal()); break;
            case BBL::Type::String: info = "string = \"" + val->stringVal()->data.substr(0, 50) + "\""; break;
            case BBL::Type::Bool: info = val->boolVal() ? "bool = true" : "bool = false"; break;
            case BBL::Type::Table: {
                BblTable* tbl = val->tableVal();
                info = "table (" + std::to_string(tbl->count) + " entries)";
                if (tbl->order) {
                    info += ": ";
                    int n = 0;
                    for (auto& k : *tbl->order) {
                        if (n++ > 5) { info += "..."; break; }
                        if (n > 1) info += ", ";
                        if (k.type() == BBL::Type::String) info += k.stringVal()->data;
                        else info += std::to_string(k.intVal());
                    }
                }
                break;
            }
            case BBL::Type::Vector: info = "vector<" + val->vectorVal()->elemType + "> length=" + std::to_string(val->vectorVal()->length()); break;
            case BBL::Type::Binary: info = "binary length=" + std::to_string(val->binaryVal()->length()); break;
            case BBL::Type::Struct: info = "struct " + val->structVal()->desc->name; break;
            case BBL::Type::Fn:
                if (val->isClosure()) info = "fn(" + std::to_string(val->closureVal()->arity) + " args)";
                else if (val->isCFn()) info = "builtin function";
                else info = "function";
                break;
            default: info = "null"; break;
            }
            result = yyjson_mut_obj(doc);
            yyjson_mut_val* contents = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, contents, "kind", "markdown");
            yyjson_mut_obj_add_strcpy(doc, contents, "value", info.c_str());
            yyjson_mut_obj_add_val(doc, result, "contents", contents);
        } else {
            result = yyjson_mut_null(doc);
        }
    } else {
        result = yyjson_mut_null(doc);
    }

    std::string resp = makeResponse(id, doc, result);
    yyjson_mut_doc_free(doc);
    return resp;
}

void lspMain() {
    while (true) {
        std::string msg = readMessage();
        if (msg.empty()) break;

        yyjson_doc* jdoc = yyjson_read(msg.c_str(), msg.size(), 0);
        if (!jdoc) continue;
        yyjson_val* root = yyjson_doc_get_root(jdoc);
        std::string method = jsonStr(yyjson_obj_get(root, "method"));
        yyjson_val* idVal = yyjson_obj_get(root, "id");
        int id = idVal ? yyjson_get_int(idVal) : -1;
        yyjson_val* params = yyjson_obj_get(root, "params");

        if (method == "initialize") {
            yyjson_mut_doc* rdoc = yyjson_mut_doc_new(nullptr);
            yyjson_mut_val* result = yyjson_mut_obj(rdoc);
            yyjson_mut_val* caps = yyjson_mut_obj(rdoc);
            yyjson_mut_obj_add_int(rdoc, caps, "textDocumentSync", 1);

            yyjson_mut_val* compProv = yyjson_mut_obj(rdoc);
            yyjson_mut_val* triggers = yyjson_mut_arr(rdoc);
            yyjson_mut_arr_add_str(rdoc, triggers, "(");
            yyjson_mut_arr_add_str(rdoc, triggers, ":");
            yyjson_mut_arr_add_str(rdoc, triggers, ".");
            yyjson_mut_obj_add_val(rdoc, compProv, "triggerCharacters", triggers);
            yyjson_mut_obj_add_val(rdoc, caps, "completionProvider", compProv);

            yyjson_mut_obj_add_bool(rdoc, caps, "hoverProvider", true);
            yyjson_mut_obj_add_bool(rdoc, caps, "definitionProvider", true);
            yyjson_mut_obj_add_val(rdoc, result, "capabilities", caps);
            sendMessage(makeResponse(id, rdoc, result));
            yyjson_mut_doc_free(rdoc);
        } else if (method == "initialized") {
            // no response needed
        } else if (method == "textDocument/didOpen") {
            yyjson_val* td = yyjson_obj_get(params, "textDocument");
            std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
            std::string text = jsonStr(yyjson_obj_get(td, "text"));
            auto analysis = analyzeDocument(text, uri);
            documents[uri] = {uri, text, std::move(analysis)};
            sendMessage(publishDiagnostics(uri, text));
        } else if (method == "textDocument/didChange") {
            yyjson_val* td = yyjson_obj_get(params, "textDocument");
            std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
            yyjson_val* changes = yyjson_obj_get(params, "contentChanges");
            yyjson_val* first = yyjson_arr_get_first(changes);
            std::string text = jsonStr(yyjson_obj_get(first, "text"));
            auto analysis = analyzeDocument(text, uri);
            auto it = documents.find(uri);
            if (it != documents.end()) {
                it->second.text = text;
                bool hasUserGlobals = false;
                if (analysis && analysis->vm) {
                    for (auto& [id, v] : analysis->vm->globals) {
                        if (v.type() == BBL::Type::Table || v.type() == BBL::Type::Struct ||
                            (v.type() == BBL::Type::Fn && v.isClosure())) {
                            hasUserGlobals = true; break;
                        }
                    }
                }
                if (hasUserGlobals) it->second.analysis = std::move(analysis);
            } else {
                documents[uri] = {uri, text, std::move(analysis)};
            }
            sendMessage(publishDiagnostics(uri, text));
        } else if (method == "textDocument/didClose") {
            yyjson_val* td = yyjson_obj_get(params, "textDocument");
            std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
            documents.erase(uri);
        } else if (method == "textDocument/completion") {
            sendMessage(handleCompletion(id, params));
        } else if (method == "textDocument/hover") {
            sendMessage(handleHover(id, params));
        } else if (method == "shutdown") {
            yyjson_mut_doc* rdoc = yyjson_mut_doc_new(nullptr);
            sendMessage(makeResponse(id, rdoc, yyjson_mut_null(rdoc)));
            yyjson_mut_doc_free(rdoc);
        } else if (method == "exit") {
            yyjson_doc_free(jdoc);
            break;
        } else if (id >= 0) {
            yyjson_mut_doc* rdoc = yyjson_mut_doc_new(nullptr);
            sendMessage(makeResponse(id, rdoc, yyjson_mut_null(rdoc)));
            yyjson_mut_doc_free(rdoc);
        }

        yyjson_doc_free(jdoc);
    }
}
