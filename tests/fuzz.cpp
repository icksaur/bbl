#include "bbl.h"

using namespace bbl;


extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096) return 0;
    try {
        BblState bbl;
        addStdLib(bbl);
        bbl.maxSteps = 10000;
        bbl.exec(std::string(reinterpret_cast<const char*>(data), size));
    } catch (...) {}
    return 0;
}
