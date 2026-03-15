#pragma once

#include "chunk.h"

namespace bbl {

void disassembleChunk(const Chunk& chunk, const char* name);
int disassembleInstruction(const Chunk& chunk, int offset);


} // namespace bbl