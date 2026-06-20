#include "../include/stdio.h"
#include "../include/abi/syscall.h"

// raw syscall
extern uint64_t syscall(uint64_t num, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);

void print(const char* str)
{
    syscall(SYS_WRITE, (uint64_t)str, 0, 0);
}

void clear() 
{
    syscall(SYS_CLEAR, 0, 0, 0);
}

void set_cursor(uint32_t x, uint32_t y)
{
    syscall(SYS_SET_CURSOR, (uint64_t)x, (uint64_t)y, 0);
}

void exit(int code)
{
    syscall(SYS_EXIT, (uint64_t)code, 0, 0);
    while (1) {}
}

keyboard_event_t read_key()
{
    keyboard_event_t e;
    syscall(SYS_READ_KEY, (uint64_t)&e, 0, 0);
    return e;
}