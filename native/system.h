#ifndef LUX_NATIVE_SYSTEM_H
#define LUX_NATIVE_SYSTEM_H

#include "native.h"

extern bool _time(int argCount, Value* args);
extern bool _clock(int argCount, Value* args);
extern bool _sleep(int argCount, Value* args);
extern bool _usleep(int argCount, Value* args);
extern bool _print(int argCount, Value* args);
extern bool _println(int argCount, Value* args);
extern bool _sprint(int argCount, Value* args);
extern bool _len(int argCount, Value* args);

#endif