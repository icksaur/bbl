#include "lsp.h"
#include "bbl.h"
#include <yyjson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

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

struct LspDoc {
    std::string uri;
    std::string text;
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

static std::string handleCompletion(int id, yyjson_val* params) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* items = yyjson_mut_arr(doc);

    yyjson_val* pos = yyjson_obj_get(params, "position");
    yyjson_val* td = yyjson_obj_get(params, "textDocument");
    std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
    int line = yyjson_get_int(yyjson_obj_get(pos, "line"));
    int ch = yyjson_get_int(yyjson_obj_get(pos, "character"));

    bool afterColon = false;
    auto dit = documents.find(uri);
    if (dit != documents.end() && ch > 0) {
        const std::string& text = dit->second.text;
        int curLine = 0, idx = 0;
        for (; idx < (int)text.size() && curLine < line; idx++)
            if (text[idx] == '\n') curLine++;
        idx += ch - 1;
        if (idx >= 0 && idx < (int)text.size() && text[idx] == ':')
            afterColon = true;
    }

    if (afterColon) {
        for (int i = 0; METHODS[i]; i++) {
            yyjson_mut_val* item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "label", METHODS[i]);
            yyjson_mut_obj_add_int(doc, item, "kind", 2);
            yyjson_mut_arr_add_val(items, item);
        }
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
            documents[uri] = {uri, text};
            sendMessage(publishDiagnostics(uri, text));
        } else if (method == "textDocument/didChange") {
            yyjson_val* td = yyjson_obj_get(params, "textDocument");
            std::string uri = jsonStr(yyjson_obj_get(td, "uri"));
            yyjson_val* changes = yyjson_obj_get(params, "contentChanges");
            yyjson_val* first = yyjson_arr_get_first(changes);
            std::string text = jsonStr(yyjson_obj_get(first, "text"));
            documents[uri] = {uri, text};
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
