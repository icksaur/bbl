#include "bbl.h"
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static void printVersion() {
    fputs("bbl 0.1.0\n", stdout);
}

static void printUsage() {
    fputs("usage: bbl [options] [script.bbl] [args...]\n"
          "  -e <code>   evaluate code string (may be repeated)\n"
          "  -v          print version\n"
          "  -h          print this help\n", stdout);
}

static void printValue(const BblValue& v) {
    char buf[64];
    switch (v.type) {
        case BBL::Type::Null: break; // silent
        case BBL::Type::Int:
            snprintf(buf, sizeof(buf), "%" PRId64, v.intVal);
            fputs(buf, stdout);
            fputc('\n', stdout);
            break;
        case BBL::Type::Float:
            snprintf(buf, sizeof(buf), "%g", v.floatVal);
            fputs(buf, stdout);
            fputc('\n', stdout);
            break;
        case BBL::Type::Bool:
            fputs(v.boolVal ? "true" : "false", stdout);
            fputc('\n', stdout);
            break;
        case BBL::Type::String:
            fprintf(stdout, "\"%s\"\n", v.stringVal->data.c_str());
            break;
        case BBL::Type::Fn:
            fputs("<fn>\n", stdout);
            break;
        case BBL::Type::Binary:
            fprintf(stdout, "<binary %zu bytes>\n", v.binaryVal->length());
            break;
        case BBL::Type::Table:
            fprintf(stdout, "<table length=%zu>\n", v.tableVal->length());
            break;
        case BBL::Type::Vector:
            fprintf(stdout, "<vector %s length=%zu>\n",
                    v.vectorVal->elemType.c_str(), v.vectorVal->length());
            break;
        case BBL::Type::Struct:
            fprintf(stdout, "<struct %s>\n", v.structVal->desc->name.c_str());
            break;
        case BBL::Type::UserData:
            fprintf(stdout, "<userdata %s>\n", v.userdataVal->desc->name.c_str());
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
    std::string input;
    int depth = 0;

    fputs("> ", stdout);
    fflush(stdout);

    std::string line;
    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            fputc('\n', stdout);
            break;
        }
        if (c == '\n') {
            if (!input.empty()) input += '\n';
            input += line;
            depth += countParenBalance(line);
            line.clear();
            if (depth <= 0 && !input.empty()) {
                // Evaluate
                std::string trimmed = input;
                // Skip whitespace-only input
                bool allWs = true;
                for (char ch : trimmed) if (!isspace(ch)) { allWs = false; break; }
                if (!allWs) {
                    try {
                        BblValue result = bbl.execExpr(trimmed);
                        printValue(result);
                    } catch (const BBL::Error& e) {
                        bbl.printBacktrace(e.what);
                    }
                }
                input.clear();
                depth = 0;
                fputs("> ", stdout);
                fflush(stdout);
            } else {
                fputs(". ", stdout);
                fflush(stdout);
            }
        } else {
            line += static_cast<char>(c);
        }
    }
}

int main(int argc, char* argv[]) {
    BblState bbl;
    BBL::addStdLib(bbl);
    bbl.allowOpenFilesystem = true;

    if (argc < 2) {
        repl(bbl);
        return 0;
    }

    // First pass: handle -v/-h early
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0) {
            printUsage();
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
                } catch (const BBL::Error& e) {
                    bbl.printBacktrace(e.what);
                    return 1;
                }
            }
        }
        return 0;
    }

    // Script file mode
    const char* scriptFile = argv[1];
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

        std::ifstream file(scriptPath);
        if (!file.is_open()) {
            fprintf(stderr, "bbl: cannot open %s\n", scriptFile);
            return 1;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        bbl.exec(ss.str());
    } catch (const BBL::Error& e) {
        bbl.printBacktrace(e.what);
        return 1;
    }
    return 0;
}
