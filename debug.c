#include "debug.h"
#include "object.h"

int in_loop = 0;
int loop_ends[254];
int loop_starts[254];

int moveForward(Chunk* chunk, int offset);

void disassembleChunk(Chunk* chunk, const char* name, bool flow)
{
    printf("== %s ==\n", name);

    for (int offset = 0; offset < chunk->count;) {
        uint8_t instruction = chunk->code[offset];

        if (instruction == OP_LOOP) {
            uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
            jump |= chunk->code[offset + 2];
            loop_starts[in_loop] = offset + 3 + -1 * jump;
            loop_ends[in_loop]   = offset;
            in_loop++;
        }

        offset = moveForward(chunk, offset);
    }

    for (int offset = 0; offset < chunk->count;) {
        offset = disassembleInstruction(chunk, offset, flow);
    }
}

static int constantInstruction(const char* name, Chunk* chunk, int offset)
{
    uint8_t constant = chunk->code[offset + 1];
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant]);
    printf("'\n");
    return offset + 2;
}

static int invokeInstruction(const char* name, Chunk* chunk, int offset)
{
    uint8_t constant = chunk->code[offset + 1];
    uint8_t argCount = chunk->code[offset + 2];
    printf("%-16s (%d args) %4d '", name, argCount, constant);
    printValue(chunk->constants.values[constant]);
    printf("'\n");
    return offset + 3;
}

static int simpleInstruction(const char* name, int offset)
{
    printf("%s\n", name);
    return offset + 1;
}

static int byteInstruction(const char* name, Chunk* chunk,
    int offset)
{
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset)
{
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

int in_false_jump = 0;
int false_jumps[254];

int in_jump = 0;
int jumps[254];

int loop_depth = 0;

int disassembleInstruction(Chunk* chunk, int offset, bool flow)
{
    uint8_t instruction   = chunk->code[offset];
    bool    is_false_jump = false;
    bool    is_op_jump    = false;

    if (flow) {
        if (instruction == OP_JUMP_IF_FALSE) {
            uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
            jump |= chunk->code[offset + 2];
            false_jumps[in_false_jump] = offset + 3 + 1 * jump;
            in_false_jump++;
            is_false_jump = true;
        }

        if (instruction == OP_JUMP) {
            uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
            jump |= chunk->code[offset + 2];
            jumps[in_jump] = offset + 3 + 1 * jump;
            in_jump++;
            is_op_jump = true;
        }

        if (is_false_jump) {
            if (in_false_jump > 1) {
                if (in_false_jump == 1) {
                    printf("├╼");
                } else {
                    if (in_false_jump > 2)
                        printf("┣╼");
                    else
                        printf("┟╼");
                }
            } else {
                printf("┌╼");
            }
        } else {
            bool closed_jump = false;
            int  jump_checks = in_false_jump;
            for (int i = 0; i < jump_checks; i++) {
                if (false_jumps[i] == offset) {
                    if (closed_jump) {
                        printf("\b\b");
                    }

                    if (i == in_false_jump - 1) {
                        if (in_false_jump == 1)
                            printf("└─");
                        else {
                            if (in_false_jump > 2)
                                printf("┣─");
                            else
                                printf("┡─");
                        }
                    } else {
                        if (in_false_jump - 1 > 1)
                            printf("┠─");
                        else if (in_false_jump - 1 == 1)
                            printf("┞─");
                        else
                            printf("┖─");
                    }

                    in_false_jump--;
                    closed_jump = true;
                }
            }

            if (in_false_jump >= 1 && !closed_jump) {
                if (in_false_jump >= 2) {
                    printf("┃ ");
                } else {
                    printf("│ ");
                }
            } else if (!closed_jump) {
                printf("  ");
            }
        }
    }

    printf(" %04d ", offset);

    if (flow) {
        if (is_op_jump) {
            if (in_jump > 1) {
                if (in_jump == 1) {
                    printf("╾┤");
                } else if (in_jump > 2) {
                    printf("╾┨");
                } else {
                    printf("╾┪");
                }
            } else {
                printf("╾┐");
            }
        } else {
            bool closed_jump = false;
            int  jump_checks = in_jump;
            for (int i = 0; i < jump_checks; i++) {
                if (jumps[i] == offset) {
                    if (closed_jump) {
                        printf("\b\b");
                    }

                    if (i == in_jump - 1) {
                        if (in_jump == 1)
                            printf("─┘");
                        else {
                            if (in_jump > 2)
                                printf("─┫");
                            else
                                printf("─┩");
                        }
                    } else {
                        if (in_jump - 1 > 1)
                            printf("─┫");
                        else if (in_jump - 1 == 1)
                            printf("─┪");
                        else
                            printf("─┚");
                    }

                    in_jump--;
                    closed_jump = true;
                }
            }

            if (in_jump >= 1 && !closed_jump) {
                if (in_jump >= 2) {
                    printf(" ┃");
                } else {
                    printf(" │");
                }
            } else if (!closed_jump) {
                printf("  ");
            }
        }

        // loops
        bool loop_edge = false;
        // loop through start and ends to check if we're in a loop
        for (int i = 0; i < in_loop; i++) {
            if (loop_starts[i] == offset) {
                loop_depth++;
                if (loop_depth > 1) {
                    printf("├╼");
                } else {
                    printf("┌╼");
                }
                loop_edge = true;
            }
        }

        for (int i = 0; i < in_loop; i++) {
            if (loop_ends[i] == offset) {
                if (loop_depth > 1) {
                    printf("├╼");
                } else {
                    printf("└╼");
                }
                loop_depth--;
                loop_edge = true;
            }
        }

        if (!loop_edge) {
            if (loop_depth > 0) {
                printf("│ ");
            } else {
                printf("  ");
            }
        }
    }

    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    switch (instruction) {
    case OP_CONSTANT:
        return constantInstruction("OP_CONSTANT", chunk, offset);
    case OP_NIL:
        return simpleInstruction("OP_NIL", offset);
    case OP_TRUE:
        return simpleInstruction("OP_TRUE", offset);
    case OP_FALSE:
        return simpleInstruction("OP_FALSE", offset);
    case OP_EQUAL:
        return simpleInstruction("OP_EQUAL", offset);
    case OP_GREATER:
        return simpleInstruction("OP_GREATER", offset);
    case OP_LESS:
        return simpleInstruction("OP_LESS", offset);
    case OP_ADD:
        return simpleInstruction("OP_ADD", offset);
    case OP_SUBTRACT:
        return simpleInstruction("OP_SUBTRACT", offset);
    case OP_MULTIPLY:
        return simpleInstruction("OP_MULTIPLY", offset);
    case OP_DIVIDE:
        return simpleInstruction("OP_DIVIDE", offset);
    case OP_NOT:
        return simpleInstruction("OP_NOT", offset);
    case OP_NEGATE:
        return simpleInstruction("OP_NEGATE", offset);
    case OP_INCREMENT:
        return simpleInstruction("OP_INCREMENT", offset);
    case OP_DECREMENT:
        return simpleInstruction("OP_DECREMENT", offset);
    case OP_POP:
        return simpleInstruction("OP_POP", offset);
    case OP_DUP:
        return simpleInstruction("OP_DUP", offset);
    case OP_GET_LOCAL:
        return byteInstruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:
        return byteInstruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_GLOBAL:
        return constantInstruction("OP_GET_GLOBAL", chunk, offset);
    case OP_DEFINE_GLOBAL:
        return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
    case OP_SET_GLOBAL:
        return constantInstruction("OP_SET_GLOBAL", chunk, offset);
    case OP_GET_UPVALUE:
        return byteInstruction("OP_GET_UPVALUE", chunk, offset);
    case OP_SET_UPVALUE:
        return byteInstruction("OP_SET_UPVALUE", chunk, offset);
    case OP_GET_PROPERTY:
        return constantInstruction("OP_GET_PROPERTY", chunk, offset);
    case OP_SET_PROPERTY:
        return constantInstruction("OP_SET_PROPERTY", chunk, offset);
    case OP_GET_SUPER:
        return constantInstruction("OP_GET_SUPER", chunk, offset);
    case OP_SET_TABLE:
        return byteInstruction("OP_SET_TABLE", chunk, offset);
    case OP_SET_ARRAY:
        return byteInstruction("OP_SET_ARRAY", chunk, offset);
    case OP_JUMP:
        return jumpInstruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE:
        return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:
        return jumpInstruction("OP_LOOP", -1, chunk, offset);
    case OP_DUMP:
        return simpleInstruction("OP_DUMP", offset);
    case OP_CALL:
        return byteInstruction("OP_CALL", chunk, offset);
    case OP_INDEX:
        return simpleInstruction("OP_INDEX", offset);
    case OP_SET_INDEX:
        return simpleInstruction("OP_SET_INDEX", offset);
    case OP_INVOKE:
        return invokeInstruction("OP_INVOKE", chunk, offset);
    case OP_SUPER_INVOKE:
        return invokeInstruction("OP_SUPER_INVOKE", chunk, offset);
    case OP_CLOSURE: {
        offset++;
        uint8_t constant = chunk->code[offset++];
        printf("%-16s %4d ", "OP_CLOSURE", constant);
        printValue(chunk->constants.values[constant]);
        printf("\n");

        ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
        for (int j = 0; j < function->upvalueCount; j++) {
            int isLocal = chunk->code[offset++];
            int index   = chunk->code[offset++];
            printf("%04d      |                     %s %d\n",
                offset - 2, isLocal ? "local" : "upvalue", index);
        }

        return offset;
    }
    case OP_CLOSE_UPVALUE:
        return simpleInstruction("OP_CLOSE_UPVALUE", offset);
    case OP_RETURN:
        return simpleInstruction("OP_RETURN", offset);
    case OP_CLASS:
        return constantInstruction("OP_CLASS", chunk, offset);
    case OP_INHERIT:
        return simpleInstruction("OP_INHERIT", offset);
    case OP_METHOD:
        return constantInstruction("OP_METHOD", chunk, offset);
    default:
        printf("Unknown opcode %d\n", instruction);
        return offset + 1;
    }
}

int moveForward(Chunk* chunk, int offset)
{
    uint8_t instruction = chunk->code[offset];

    switch (instruction) {
    case OP_CONSTANT:
        return offset + 2;
    case OP_NIL:
        return offset + 1;
    case OP_TRUE:
        return offset + 1;
    case OP_FALSE:
        return offset + 1;
    case OP_EQUAL:
        return offset + 1;
    case OP_GREATER:
        return offset + 1;
    case OP_LESS:
        return offset + 1;
    case OP_ADD:
        return offset + 1;
    case OP_SUBTRACT:
        return offset + 1;
    case OP_MULTIPLY:
        return offset + 1;
    case OP_DIVIDE:
        return offset + 1;
    case OP_NOT:
        return offset + 1;
    case OP_NEGATE:
        return offset + 1;
    case OP_POP:
        return offset + 1;
    case OP_DUP:
        return offset + 1;
    case OP_GET_LOCAL:
        return offset + 2;
    case OP_SET_LOCAL:
        return offset + 2;
    case OP_GET_GLOBAL:
        return offset + 2;
    case OP_DEFINE_GLOBAL:
        return offset + 2;
    case OP_SET_GLOBAL:
        return offset + 2;
    case OP_GET_UPVALUE:
        return offset + 2;
    case OP_SET_UPVALUE:
        return offset + 2;
    case OP_GET_PROPERTY:
        return offset + 2;
    case OP_SET_PROPERTY:
        return offset + 2;
    case OP_GET_SUPER:
        return offset + 2;
    case OP_SET_TABLE:
        return offset + 2;
    case OP_SET_ARRAY:
        return offset + 2;
    case OP_JUMP:
        return offset + 3;
    case OP_JUMP_IF_FALSE:
        return offset + 3;
    case OP_LOOP:
        return offset + 3;
    case OP_DUMP:
        return offset + 1;
    case OP_CALL:
        return offset + 2;
    case OP_INDEX:
        return offset + 1;
    case OP_SET_INDEX:
        return offset + 1;
    case OP_INVOKE:
        return offset + 3;
    case OP_SUPER_INVOKE:
        return offset + 3;
    case OP_CLOSURE: {
        offset++;
        uint8_t      constant = chunk->code[offset++];
        ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
        for (int j = 0; j < function->upvalueCount; j++) {
            int isLocal = chunk->code[offset++];
            int index   = chunk->code[offset++];
        }
        return offset;
    }
    case OP_CLOSE_UPVALUE:
        return offset + 1;
    case OP_RETURN:
        return offset + 1;
    case OP_CLASS:
        return offset + 2;
    case OP_INHERIT:
        return offset + 1;
    case OP_METHOD:
        return offset + 2;
    default:
        return offset + 1;
    }
}
