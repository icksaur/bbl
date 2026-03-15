#include "bbl.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace bbl;


int main(int argc, char** argv) {
    int iterations = 10000;
    if (argc > 1) iterations = atoi(argv[1]);

    unsigned seed = 42;
    auto rng = [&]() -> unsigned { seed = seed * 1103515245 + 12345; return (seed >> 16) & 0x7FFF; };

    const char* forms[] = {
        "+", "-", "*", "/", "%", "=", "==", "!=", "<", ">", "<=", ">=",
        "if", "fn", "do", "exec", "execfile",
        "binary", "vector", "table", "sizeof", "int", "not", "and", "or",
        "band", "bor", "bxor", "bnot", "shl", "shr", "print", "str", "typeof",
    };
    int nForms = sizeof(forms) / sizeof(forms[0]);

    const char* atoms[] = { "1", "0", "-1", "\"x\"", "true", "false", "null", "3.14" };
    int nAtoms = sizeof(atoms) / sizeof(atoms[0]);

    int pass = 0;
    for (int i = 0; i < iterations; i++) {
        std::string code = "(";
        code += forms[rng() % nForms];
        int nargs = rng() % 5;
        for (int j = 0; j < nargs; j++) {
            code += " ";
            if (rng() % 4 == 0) {
                code += "(";
                code += forms[rng() % nForms];
                int inner = rng() % 3;
                for (int k = 0; k < inner; k++) { code += " "; code += atoms[rng() % nAtoms]; }
                code += ")";
            } else {
                code += atoms[rng() % nAtoms];
            }
        }
        code += ")";

        try {
            BblState bbl;
            addStdLib(bbl);
            bbl.exec(code);
        } catch (...) {}
        pass++;
    }
    fprintf(stderr, "fuzz: %d/%d completed (no crashes)\n", pass, iterations);
    return 0;
}
