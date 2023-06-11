#ifndef lux_debug_h
#define lux_debug_h

#include "chunk.h"
#include "common.h"

void disassembleChunk(Chunk* chunk, const char* name);
int  disassembleInstruction(Chunk* chunk, int offset);

#endif
