#include "../include/abi/syscall.h"
#include "../include/abi/types.h"

uint64_t syscall(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2) // RAX (+return), RDI, RSI, RDX
        : "memory", "rcx", "r11"
    );
    return ret;
}