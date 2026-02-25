#include "bbl.h"
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: bbl <script.bbl>\n");
        return 2;
    }

    BblState bbl;
    try {
        bbl.execfile(argv[1]);
    } catch (const BBL::Error& e) {
        fprintf(stderr, "bbl: %s\n", e.what.c_str());
        return 1;
    }
    return 0;
}
