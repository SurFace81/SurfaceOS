#ifndef SFOS_SYSCALL_H
#define SFOS_SYSCALL_H

#include "abi/types.h"

// Raw syscall: up to 6 arguments, returns the raw kernel value.
// >= 0 on success, -errno on failure (abi/errno.h).
// Wrappers in libc/ translate that into the POSIX convention
// (-1/MAP_FAILED/NULL + errno); call this directly only for tests.
sint64_t syscall(uint64_t num,
                 uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0,
                 uint64_t arg3 = 0, uint64_t arg4 = 0, uint64_t arg5 = 0);

// Kernel convention -> POSIX convention: if r is -errno, sets `errno` and
// returns -1; otherwise returns r unchanged. Wrappers cast the result to
// their own return type.
sint64_t __syscall_ret(sint64_t r);

#endif
