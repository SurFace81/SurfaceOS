#ifndef LIB_SYSCALL_H
#define LIB_SYSCALL_H

#include "types.h"

#define SYS_EXIT  0
#define SYS_WRITE 1

uint64_t syscall(uint64_t num, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);

#endif