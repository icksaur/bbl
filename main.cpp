#include "bbl.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

static void printVersion() {
    fputs("bbl 0.1.0\n", stdout);
}

static void printUsage() {
    fputs("usage: bbl [options] [script.bbl] [args...]\n"
          "  -e <code>   evaluate code string\n"
          "  -v          print version\n"
          "  -h          print this help\n", stdout);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    BblState bbl;
    BBL::addStdLib(bbl);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0) {
            printUsage();
            return 0;
        }
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
            return 0;
        }

        // Script file
        try {
            namespace fs = std::filesystem;
            fs::path scriptPath = fs::absolute(argv[i]);
            bbl.currentFile = scriptPath.string();
            bbl.scriptDir = scriptPath.parent_path().string();
            std::ifstream file(scriptPath);
            if (!file.is_open()) {
                fprintf(stderr, "bbl: cannot open %s\n", argv[i]);
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

    return 0;
}
