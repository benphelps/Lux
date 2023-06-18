#include <time.h>
#include <unistd.h>

#include "compiler.h"
#include "debug.h"
#include "native/native.h"
#include "vm.h"

VM vm;

static void resetStack()
{
    vm.stackTop     = vm.stack;
    vm.frameCount   = 0;
    vm.openUpvalues = NULL;
}

extern void runtimeError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame*   frame       = &vm.frames[i];
        ObjFunction* function    = frame->closure->function;
        size_t       instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ",
            function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack();
}

static void defineNative(const char* name, NativeFn function)
{
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, vm.stack[0], vm.stack[1]);
    pop();
    pop();
}

static void initNative()
{
    for (NativeFnEntry* entry = nativeFns; entry->name != NULL; entry++) {
        defineNative(entry->name, entry->function);
    }
}

void initVM()
{
    resetStack();
    vm.objects = NULL;

    vm.bytesAllocated = 0;
    vm.nextGC         = 1024 * 1024;
    vm.grayCount      = 0;
    vm.grayCapacity   = 0;
    vm.grayStack      = NULL;

    initTable(&vm.globals);
    initTable(&vm.strings);

    vm.initString = NULL;
    vm.initString = copyString("init", 4);
    vm.addString  = NULL;
    vm.addString  = copyString("__add", 5);
    vm.subString  = NULL;
    vm.subString  = copyString("__sub", 5);
    vm.mulString  = NULL;
    vm.mulString  = copyString("__mul", 5);
    vm.divString  = NULL;
    vm.divString  = copyString("__div", 5);
    vm.gtString   = NULL;
    vm.gtString   = copyString("__gt", 4);
    vm.ltString   = NULL;
    vm.ltString   = copyString("__lt", 4);
    vm.eqString   = NULL;
    vm.eqString   = copyString("__eq", 4);
    vm.andString  = NULL;
    vm.andString  = copyString("__and", 5);
    vm.orString   = NULL;
    vm.orString   = copyString("__or", 4);
    vm.xorString  = NULL;
    vm.xorString  = copyString("__xor", 5);
    vm.modString  = NULL;
    vm.modString  = copyString("__mod", 5);
    vm.notString  = NULL;
    vm.notString  = copyString("__not", 5);

    initNative();
}

void freeVM()
{
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    vm.addString  = NULL;
    vm.subString  = NULL;
    vm.mulString  = NULL;
    vm.divString  = NULL;
    vm.gtString   = NULL;
    vm.ltString   = NULL;
    vm.eqString   = NULL;
    vm.andString  = NULL;
    vm.orString   = NULL;
    vm.xorString  = NULL;
    vm.modString  = NULL;
    vm.notString  = NULL;
    freeObjects();
}

void push(Value value)
{
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop()
{
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance)
{
    return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount)
{
    if (argCount != closure->function->arity) {
        runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->closure   = closure;
    frame->ip        = closure->function->chunk.code;
    frame->slots     = vm.stackTop - argCount - 1;
    return true;
}

static bool callValue(Value callee, int argCount)
{
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound      = AS_BOUND_METHOD(callee);
            vm.stackTop[-argCount - 1] = bound->receiver;
            return call(bound->method, argCount);
        }
        case OBJ_CLASS: {
            ObjClass* klass            = AS_CLASS(callee);
            vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));

            Value initializer;
            if (tableGet(&klass->methods, OBJ_VAL(vm.initString), &initializer)) {
                return call(AS_CLOSURE(initializer), argCount);
            } else if (argCount != 0) {
                runtimeError("Expected 0 arguments but got %d.", argCount);
                return false;
            }

            return true;
        }
        case OBJ_CLOSURE:
            return call(AS_CLOSURE(callee), argCount);
        case OBJ_NATIVE: {
            NativeFn native = AS_NATIVE(callee);
            Value    result = native(argCount, vm.stackTop - argCount);
            vm.stackTop -= argCount + 1;
            push(result);
            return true;
        }
        default:
            break; // Non-callable object type.
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
}

static bool invokeFromClass(ObjClass* klass, Value name, int argCount)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("1Undefined property '%s'.", stringValue(name));
        return false;
    }
    return call(AS_CLOSURE(method), argCount);
}

static bool invoke(Value name, int argCount)
{
    Value receiver = peek(argCount);

    if (!IS_INSTANCE(receiver)) {
        runtimeError("Only instances have methods.");
        return false;
    }

    ObjInstance* instance = AS_INSTANCE(receiver);

    Value value;
    if (tableGet(&instance->fields, name, &value)) {
        vm.stackTop[-argCount - 1] = value;
        return callValue(value, argCount);
    }

    return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, Value name)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        runtimeError("Undefined property '%s'.", stringValue(name));
        return false;
    }

    ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    pop();
    push(OBJ_VAL(bound));
    return true;
}

// push the indexed value of the object on the stack
bool indexValue(Value value, Value index)
{
    if (IS_OBJ(value)) {
        switch (OBJ_TYPE(value)) {
        case OBJ_STRING: {
            if (IS_NUMBER(index)) {
                int        i      = (int)AS_NUMBER(index);
                ObjString* string = AS_STRING(value);
                if (i < 0 || i >= string->length) {
                    runtimeError("String index out of bounds.");
                    return false;
                }
                push(OBJ_VAL(copyString(&string->chars[i], 1)));
                return true;
            }
            break;
        }
        case OBJ_TABLE: {
            ObjTable* table = AS_TABLE(value);
            Value     entry;
            if (tableGet(&table->table, index, &entry)) {
                push(entry);
                return true;
            } else {
                runtimeError("Undefined table property '%s'.", stringValue(index));
                return false;
            }
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* array = AS_ARRAY(value);
            if (IS_NUMBER(index)) {
                int i = (int)AS_NUMBER(index);
                if (i < 0 || i >= array->array.count) {
                    runtimeError("Array index out of bounds.");
                    return false;
                }
                Value entry = array->array.values[i];
                push(entry);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    runtimeError("Only strings, tables and arrays can be indexed.");
    return false;
}

static ObjUpvalue* captureUpvalue(Value* local)
{
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue     = vm.openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue     = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next       = upvalue;

    if (prevUpvalue == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(Value* last)
{
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue* upvalue = vm.openUpvalues;
        upvalue->closed     = *upvalue->location;
        upvalue->location   = &upvalue->closed;
        vm.openUpvalues     = upvalue->next;
    }
}

static void defineMethod(ObjString* name)
{
    Value     method = peek(0);
    ObjClass* klass  = AS_CLASS(peek(1));
    tableSet(&klass->methods, OBJ_VAL(name), method);
    pop();
}

static void defineProperty(ObjString* name)
{
    Value     field = peek(0);
    ObjClass* klass = AS_CLASS(peek(1));
    tableSet(&klass->fields, OBJ_VAL(name), field);
    pop();
}

static bool isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate()
{
    ObjString* b = AS_STRING(peek(0));
    ObjString* a = AS_STRING(peek(1));

    int   length = a->length + b->length;
    char* chars  = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = takeString(chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

static InterpretResult run()
{
    CallFrame* frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op)                          \
    do {                                                  \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtimeError("Operands must be numbers.");    \
            return INTERPRET_RUNTIME_ERROR;               \
        }                                                 \
        double b = AS_NUMBER(pop());                      \
        double a = AS_NUMBER(pop());                      \
        push(valueType(a op b));                          \
    } while (false)

#define BINARY_OP_INT(valueType, op)                      \
    do {                                                  \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtimeError("Operands must be numbers.");    \
            return INTERPRET_RUNTIME_ERROR;               \
        }                                                 \
        int b = (int)AS_NUMBER(pop());                    \
        int a = (int)AS_NUMBER(pop());                    \
        push(valueType(a op b));                          \
    } while (false)

#define INVOKE_DUNDER(dunderMethod)                                            \
    Obj* this  = AS_OBJ(peek(1));                                              \
    Obj* other = AS_OBJ(peek(0));                                              \
    if (this->type == OBJ_INSTANCE && other->type == OBJ_INSTANCE) {           \
        ObjInstance* thisInstance  = (ObjInstance*)this;                       \
        ObjInstance* otherInstance = (ObjInstance*)other;                      \
        if (thisInstance->klass != otherInstance->klass) {                     \
            runtimeError("Operands must be two instances of the same class."); \
            return INTERPRET_RUNTIME_ERROR;                                    \
        }                                                                      \
        Value method   = OBJ_VAL(dunderMethod);                                \
        int   argCount = 1;                                                    \
        if (!invoke(method, argCount)) {                                       \
            return INTERPRET_RUNTIME_ERROR;                                    \
        }                                                                      \
        frame = &vm.frames[vm.frameCount - 1];                                 \
    }

    for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
        if (vm.stackTop != vm.stack) {
            printf("           ");
        }
        for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        disassembleInstruction(&frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code), false);
#endif
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
        case OP_CONSTANT: {
            Value constant = READ_CONSTANT();
            push(constant);
            break;
        }
        case OP_NIL:
            push(NIL_VAL);
            break;
        case OP_TRUE:
            push(BOOL_VAL(true));
            break;
        case OP_FALSE:
            push(BOOL_VAL(false));
            break;
        case OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            push(*frame->closure->upvalues[slot]->location);
            break;
        }
        case OP_SET_UPVALUE: {
            uint8_t slot                              = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(0);
            break;
        }
        case OP_EQUAL: {
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.eqString);
            } else {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
            }
            break;
        }
        case OP_GREATER:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.gtString);
            } else {
                BINARY_OP(BOOL_VAL, >);
            }
            break;
        case OP_LESS:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.ltString);
            } else {
                BINARY_OP(BOOL_VAL, <);
            }
            break;
        case OP_ADD: {
            if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                concatenate();
            } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                double b = AS_NUMBER(pop());
                double a = AS_NUMBER(pop());
                push(NUMBER_VAL(a + b));
            } else if (IS_TABLE(peek(0)) && IS_TABLE(peek(1))) {
                ObjTable* b   = AS_TABLE(pop());
                ObjTable* a   = AS_TABLE(pop());
                ObjTable* new = newTable();
                tableAddAll(&b->table, &new->table);
                tableAddAll(&a->table, &new->table);
                push(OBJ_VAL(new));
            } else if (IS_ARRAY(peek(0)) && IS_ARRAY(peek(1))) {
                ObjArray* b   = AS_ARRAY(pop());
                ObjArray* a   = AS_ARRAY(pop());
                ObjArray* new = newArray();
                joinValueArray(&new->array, &a->array);
                joinValueArray(&new->array, &b->array);
                push(OBJ_VAL(new));
            } else if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.addString);
            } else {
                runtimeError(
                    "Operands must be two joinable types.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_SUBTRACT:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.subString);
            } else {
                BINARY_OP(NUMBER_VAL, -);
            }
            break;
        case OP_MULTIPLY:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.mulString);
            } else {
                BINARY_OP(NUMBER_VAL, *);
            }
            break;
        case OP_DIVIDE:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.divString);
            } else {
                BINARY_OP(NUMBER_VAL, /);
            }
            break;
        case OP_MODULO:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.modString);
            } else {
                BINARY_OP_INT(NUMBER_VAL, %);
            }
            break;
        case OP_BITWISE_AND:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.andString);
            } else {
                BINARY_OP_INT(NUMBER_VAL, &);
            }
            break;
        case OP_BITWISE_OR:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.orString);
            } else {
                BINARY_OP_INT(NUMBER_VAL, |);
            }
            break;
        case OP_BITWISE_XOR:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.xorString);
            } else {
                BINARY_OP_INT(NUMBER_VAL, ^);
            }
            break;
        case OP_SHIFT_LEFT:
            BINARY_OP_INT(NUMBER_VAL, <<);
            break;
        case OP_SHIFT_RIGHT:
            BINARY_OP_INT(NUMBER_VAL, >>);
            break;
        case OP_NOT:
            if (IS_INSTANCE(peek(0)) && IS_INSTANCE(peek(1))) {
                INVOKE_DUNDER(vm.notString);
            } else {
                push(BOOL_VAL(isFalsey(pop())));
            }
            break;
        case OP_NEGATE: {
            if (!IS_NUMBER(peek(0))) {
                runtimeError("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(-AS_NUMBER(pop())));
            break;
        }
        case OP_INCREMENT:
            if (!IS_NUMBER(peek(0))) {
                runtimeError("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(AS_NUMBER(pop()) + 1));
            break;
        case OP_DECREMENT:
            if (!IS_NUMBER(peek(0))) {
                runtimeError("Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(AS_NUMBER(pop()) - 1));
            break;
        case OP_POP:
            pop();
            break;
        case OP_DUP: {
            push(peek(0));
            break;
        }
        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();
            push(frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL: {
            uint8_t slot       = READ_BYTE();
            frame->slots[slot] = peek(0);
            break;
        }
        case OP_GET_GLOBAL: {
            Value name = READ_CONSTANT();
            Value value;
            if (!tableGet(&vm.globals, name, &value)) {
                runtimeError("Undefined variable '%s'.", stringValue(name));
                return INTERPRET_RUNTIME_ERROR;
            }
            push(value);
            break;
        }
        case OP_DEFINE_GLOBAL: {
            Value name = READ_CONSTANT();
            tableSet(&vm.globals, name, peek(0));
            pop();
            break;
        }
        case OP_SET_GLOBAL: {
            Value name = READ_CONSTANT();
            if (tableSet(&vm.globals, name, peek(0))) {
                tableDelete(&vm.globals, name);
                runtimeError("Undefined variable '%s'.", stringValue(name));
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_GET_PROPERTY: {
            Obj* value = AS_OBJ(peek(0)); // break
            switch (value->type) {
            case OBJ_INSTANCE: {
                ObjInstance* instance = AS_INSTANCE(peek(0));
                Value        name     = READ_CONSTANT();

                Value value;
                if (tableGet(&instance->fields, name, &value)) {
                    pop(); // Instance.
                    push(value);
                    break;
                }

                if (!bindMethod(instance->klass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                break;
            }
            case OBJ_TABLE: {
                ObjTable* table = AS_TABLE(peek(0));
                Value     index = READ_CONSTANT();
                Value     value;
                if (!tableGet(&table->table, index, &value)) {
                    runtimeError("3Undefined property '%s'.", AS_STRING(index)->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                pop();
                push(value);
                break;
            }

            default: {
                runtimeError("Only instances and tables have properties.");
                return INTERPRET_RUNTIME_ERROR;
            }
            }
            break;
        }
        case OP_SET_PROPERTY: {
            Obj* value = AS_OBJ(peek(1));

            switch (value->type) {
            case OBJ_INSTANCE: {
                ObjInstance* instance = AS_INSTANCE(peek(1));
                tableSet(&instance->fields, READ_CONSTANT(), peek(0));
                Value value = pop();
                pop();
                push(value);
                break;
            }

            case OBJ_TABLE: {
                ObjTable* table = AS_TABLE(peek(1));
                tableSet(&table->table, READ_CONSTANT(), peek(0));
                Value value = pop();
                pop();
                push(value);
                break;
            }

            default:
                runtimeError("Only instances and tables have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }

            break;
        }
        case OP_GET_SUPER: {
            Value     name       = READ_CONSTANT();
            ObjClass* superclass = AS_CLASS(pop());

            if (!bindMethod(superclass, name)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_JUMP: {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_SHORT();
            if (isFalsey(peek(0)))
                frame->ip += offset;
            break;
        }
        case OP_LOOP: {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }
        case OP_DUMP: {
            printValue(pop());
            printf("\n");
            break;
        }
        case OP_CALL: {
            int argCount = READ_BYTE();
            if (!callValue(peek(argCount), argCount)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_INDEX: {
            Value index = pop();
            Value value = pop();
            if (!indexValue(value, index)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_SET_INDEX: {
            Obj* value = AS_OBJ(peek(2));

            switch (value->type) {
            case OBJ_TABLE: {
                Value     value = pop();
                Value     index = pop();
                ObjTable* table = AS_TABLE(pop());
                tableSet(&table->table, index, value);
                push(OBJ_VAL(table));
                break;
            }

            case OBJ_ARRAY: {
                Value     value = pop();
                Value     index = pop();
                ObjArray* array = AS_ARRAY(pop());
                if (!IS_NUMBER(index)) {
                    runtimeError("Index must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (AS_NUMBER(index) < 0 || AS_NUMBER(index) >= array->array.count) {
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                array->array.values[(int)AS_NUMBER(index)] = value;
                push(OBJ_VAL(array));
                break;
            }

            case OBJ_STRING: {
                Value      value  = pop();
                Value      index  = pop();
                ObjString* string = AS_STRING(pop());
                if (!IS_NUMBER(index)) {
                    runtimeError("Index must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (AS_NUMBER(index) < 0 || AS_NUMBER(index) >= string->length) {
                    runtimeError("Index out of bounds.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!IS_STRING(value)) {
                    runtimeError("Value must be a character.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (AS_STRING(value)->length != 1) {
                    runtimeError("Value must be a character.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                string->chars[(int)AS_NUMBER(index)] = AS_STRING(value)->chars[0];
                push(OBJ_VAL(string));
                break;
            }

            default: {
                runtimeError("Only strings, tables and arrays have indexes.");
                return INTERPRET_RUNTIME_ERROR;
            }
            }

            break;
        }
        case OP_INVOKE: {
            Value method   = READ_CONSTANT();
            int   argCount = READ_BYTE();
            if (!invoke(method, argCount)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_SUPER_INVOKE: {
            Value     method     = READ_CONSTANT();
            int       argCount   = READ_BYTE();
            ObjClass* superclass = AS_CLASS(pop());
            if (!invokeFromClass(superclass, method, argCount)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_CLOSURE: {
            ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
            ObjClosure*  closure  = newClosure(function);

            push(OBJ_VAL(closure));

            for (int i = 0; i < closure->upvalueCount; i++) {
                uint8_t isLocal = READ_BYTE();
                uint8_t index   = READ_BYTE();
                if (isLocal) {
                    closure->upvalues[i] = captureUpvalue(frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }
        case OP_CLOSE_UPVALUE:
            closeUpvalues(vm.stackTop - 1);
            pop();
            break;
        case OP_SET_TABLE: {
            int       elemsCount = READ_BYTE();
            ObjTable* table      = newTable();

            if (elemsCount > 0) {
                for (int i = elemsCount - 1; i >= 0; i--) {
                    // if (!IS_STRING(peek(1))) {
                    //     runtimeError("Table key must be a string.");
                    //     return INTERPRET_RUNTIME_ERROR;
                    // }
                    tableSet(&table->table, peek(1), peek(0));
                    pop();
                    pop();
                }
            }

            push(OBJ_VAL(table));
            break;
        }
        case OP_SET_ARRAY: {
            int       elemsCount = READ_BYTE();
            ObjArray* array      = newArray();

            if (elemsCount > 0) {
                for (int i = elemsCount - 1; i >= 0; i--) {
                    writeValueArray(&array->array, peek(i));
                }

                for (int i = elemsCount - 1; i >= 0; i--) {
                    pop();
                }
            }

            push(OBJ_VAL(array));
            break;
        }
        case OP_RETURN: {
            Value result = pop();
            closeUpvalues(frame->slots);
            vm.frameCount--;
            if (vm.frameCount == 0) {
                pop();
                return INTERPRET_OK;
            }

            vm.stackTop = frame->slots;
            push(result);
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_CLASS:
            push(OBJ_VAL(newClass(READ_STRING())));
            break;
        case OP_INHERIT: {
            Value superclass = peek(1);

            if (!IS_CLASS(superclass)) {
                runtimeError("Superclass must be a class.");
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjClass* subclass = AS_CLASS(peek(0));
            tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
            pop(); // Subclass.
            break;
        }
        case OP_METHOD:
            defineMethod(READ_STRING());
            break;
        case OP_PROPERTY:
            defineProperty(READ_STRING());
            break;
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char* source)
{
    ObjFunction* function = compile(source);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));

    ObjClosure* closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}
