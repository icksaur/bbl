#include "bbl.h"
#include "dap.h"
#include "lsp.h"
#include <linenoise.h>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>

namespace bbl {


static void printVersion() {
    fputs("bbl 0.1.0\n", stdout);
}

static void printUsage() {
    fputs("usage: bbl [options] [script.bbl] [args...]\n"
          "  -e <code>     evaluate code string (may be repeated)\n"
          "  --compress    compress binary literals in stdin to stdout\n"
          "  --dap <port>  run as DAP debug server on given port\n"
          "  -v            print version\n"
          "  -h            print this help\n", stdout);
}

static void printValue(const BblValue& v) {
    char buf[64];
    switch (v.type()) {
        case Type::Null: break; // silent
        case Type::Int:
            snprintf(buf, sizeof(buf), "%" PRId64, v.intVal());
            fputs(buf, stdout);
            fputc('\n', stdout);
            break;
        case Type::Float:
            snprintf(buf, sizeof(buf), "%g", v.floatVal());
            fputs(buf, stdout);
            fputc('\n', stdout);
            break;
        case Type::Bool:
            fputs(v.boolVal() ? "true" : "false", stdout);
            fputc('\n', stdout);
            break;
        case Type::String:
            fprintf(stdout, "\"%s\"\n", v.stringVal()->data.c_str());
            break;
        case Type::Fn:
            fputs("<fn>\n", stdout);
            break;
        case Type::Binary:
            fprintf(stdout, "<binary %zu bytes>\n", v.binaryVal()->length());
            break;
        case Type::Table:
            fprintf(stdout, "<table length=%zu>\n", v.tableVal()->length());
            break;
        case Type::Vector:
            fprintf(stdout, "<vector %s length=%zu>\n",
                    v.vectorVal()->elemType.c_str(), v.vectorVal()->length());
            break;
        case Type::Struct:
            fprintf(stdout, "<struct %s>\n", v.structVal()->desc->name.c_str());
            break;
        case Type::UserData:
            fprintf(stdout, "<userdata %s>\n", v.userdataVal()->desc->name.c_str());
            break;
        default: break;
    }
}

static int countParenBalance(const std::string& line) {
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (inString) {
            if (c == '\\' && i + 1 < line.size()) { i++; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '/' && i + 1 < line.size() && line[i+1] == '/') break;
        if (c == '"') { inString = true; continue; }
        if (c == '(') depth++;
        else if (c == ')') depth--;
    }
    return depth;
}

static void repl(BblState& bbl) {
    std::string historyFile;
    if (const char* home = getenv("HOME")) {
        historyFile = std::string(home) + "/.bbl_history";
        linenoiseHistoryLoad(historyFile.c_str());
    }
    linenoiseSetMultiLine(1);

    std::string input;
    int depth = 0;

    while (true) {
        const char* prompt = depth > 0 ? ". " : "> ";
        char* raw = linenoise(prompt);
        if (!raw) break;
        std::string line(raw);
        linenoiseFree(raw);

        if (!input.empty()) input += '\n';
        input += line;
        depth += countParenBalance(line);

        if (depth <= 0 && !input.empty()) {
            bool allWs = true;
            for (char ch : input) if (!isspace(ch)) { allWs = false; break; }
            if (!allWs) {
                linenoiseHistoryAdd(input.c_str());
                try {
                    BblValue result = bbl.execExpr(input);
                    printValue(result);
                } catch (const Error& e) {
                    bbl.printBacktrace(e.what);
                }
            }
            input.clear();
            depth = 0;
        }
    }

    if (!historyFile.empty()) linenoiseHistorySave(historyFile.c_str());
}



} // namespace bbl

using namespace bbl;

int main(int argc, char* argv[]) {
    BblState bbl;
    addStdLib(bbl);
    bbl.allowOpenFilesystem = true;

    if (argc < 2) {
        repl(bbl);
        return 0;
    }

    // First pass: handle -v/-h/--compress early
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        }
        if (strcmp(argv[i], "--lsp") == 0) {
            lspMain();
            return 0;
        }
        if (strcmp(argv[i], "--dap") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bbl: --dap requires a port number\n");
                return 2;
            }
            int dapPort = std::atoi(argv[++i]);
            if (i + 1 >= argc) {
                fprintf(stderr, "bbl: --dap requires a script file\n");
                return 2;
            }
            const char* script = argv[++i];
            try {
                namespace fs = std::filesystem;
                fs::path scriptPath = fs::absolute(script);
                bbl.currentFile = scriptPath.string();
                bbl.scriptDir = scriptPath.parent_path().string();
                bbl.allowOpenFilesystem = true;
                auto* dap = new DapServer();
                dap->state = &bbl;
                bbl.dapServer = dap;
                dap->start(dapPort);
                fprintf(stderr, "DAP server listening on port %d, waiting for debugger...\n", dapPort);
                std::ifstream file(scriptPath, std::ios::binary);
                if (!file.is_open()) {
                    fprintf(stderr, "bbl: cannot open %s\n", script);
                    return 1;
                }
                std::ostringstream ss;
                ss << file.rdbuf();
                bbl.exec(ss.str());
                if (bbl.debug) {
                    bbl.debug->scriptDone.store(true, std::memory_order_release);
                    bbl.debug->cv.notify_all();
                }
            } catch (const Error& e) {
                bbl.printBacktrace(e.what);
                return 1;
            }
            return 0;
        }
        if (strcmp(argv[i], "--compress") == 0) {
            std::string source((std::istreambuf_iterator<char>(std::cin)),
                               std::istreambuf_iterator<char>());
            try {
                BblLexer lexer(source.c_str(), source.size());
                int prevEnd = 0;
                Token tok = lexer.nextToken();
                while (tok.type != TokenType::Eof) {
                    if (tok.type == TokenType::Binary && !tok.isCompressed && tok.binarySource) {
                        fwrite(source.c_str() + prevEnd, 1, tok.sourceStart - prevEnd, stdout);
                        auto comp = lz4Compress(
                            reinterpret_cast<const uint8_t*>(tok.binarySource), tok.binarySize);
                        fprintf(stdout, "0z%zu:", comp.size());
                        fwrite(comp.data(), 1, comp.size(), stdout);
                        prevEnd = tok.sourceEnd;
                    }
                    tok = lexer.nextToken();
                }
                fwrite(source.c_str() + prevEnd, 1, source.size() - prevEnd, stdout);
            } catch (...) {
                fwrite(source.c_str(), 1, source.size(), stdout);
            }
            return 0;
        }
    }

    // Check if we have -e flags
    bool hasExecFlag = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) { hasExecFlag = true; break; }
    }

    if (hasExecFlag) {
        // Process all -e flags in the same environment
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "bbl: -e requires an argument\n");
                    return 2;
                }
                i++;
                try {
                    bbl.exec(argv[i]);
                } catch (const Error& e) {
                    bbl.printBacktrace(e.what);
                    return 1;
                }
            }
        }
        return 0;
    }

    // Script file mode
    const char* scriptFile = nullptr;
    for (int i = 1; i < argc; i++) {
        scriptFile = argv[i];
        break;
    }
    if (!scriptFile) {
        repl(bbl);
        return 0;
    }
    try {
        namespace fs = std::filesystem;
        fs::path scriptPath = fs::absolute(scriptFile);
        bbl.currentFile = scriptPath.string();
        bbl.scriptDir = scriptPath.parent_path().string();

        // Build args table from remaining argv
        BblTable* argsTable = bbl.allocTable();
        for (int i = 2; i < argc; i++) {
            BblValue key = BblValue::makeInt(static_cast<int64_t>(i - 2));
            BblValue val = BblValue::makeString(bbl.intern(argv[i]));
            argsTable->set(key, val);
        }
        bbl.set("args", BblValue::makeTable(argsTable));

        std::ifstream file(scriptPath, std::ios::binary);
        if (!file.is_open()) {
            fprintf(stderr, "bbl: cannot open %s\n", scriptFile);
            return 1;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        bbl.exec(ss.str());
    } catch (const Error& e) {
        bbl.printBacktrace(e.what);
        return 1;
    }
    return 0;
}
