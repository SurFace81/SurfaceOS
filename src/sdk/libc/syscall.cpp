#include "../include/syscall.h"
#include "../include/errno.h"
#include "../include/abi/syscall.h"

// The libc wrappers report failures the POSIX way; `errno` is theirs.
int errno = 0;

sint64_t syscall(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                 uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    // The Linux argument order puts args 4..6 in r10, r8, r9 (rcx is lost to
    // `syscall`; `int 0x80` clobbers nothing but we list rcx/r11 anyway).
    // GCC cannot name those registers in constraints, so register variables
    // pin the values - the same trick musl uses.
    register uint64_t r10 asm("r10") = arg3;
    register uint64_t r8  asm("r8")  = arg4;
    register uint64_t r9  asm("r9")  = arg5;

    sint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2),
          "r"(r10), "r"(r8), "r"(r9)
        : "memory", "rcx", "r11"
    );
    return ret;
}

// Translate the kernel convention (>= 0, -errno) into the POSIX one
// (-1 + errno). Wrappers cast the result to their return type.
sint64_t __syscall_ret(sint64_t r)
{
    if (r < 0 && r > -4096)
    {
        errno = (int)-r;
        return -1;
    }
    return r;
}

// Program break. new_brk == 0 queries the current break.
// brk has no errno convention: it always returns the (possibly unchanged)
// current break, and the caller compares it against the request.
uint64_t brk(uint64_t new_brk)
{
    return (uint64_t)syscall(SYS_BRK, new_brk);
}
