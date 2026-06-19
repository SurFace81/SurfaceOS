#include "./include/stdio.h"
#include "./include/syscall.h"

void print(const char* str)
{
    syscall(SYS_WRITE, (uint64_t)str, 0, 0);
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