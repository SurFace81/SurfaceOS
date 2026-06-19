#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_READ_KEY  2

extern "C" void syscall_entry();

#endif