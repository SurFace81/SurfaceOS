#ifndef SYS_FS_H
#define SYS_FS_H

#include "process.h"

// File-descriptor syscalls (stage 3.6): Linux numbers, -errno results.
// Every handler validates user pointers through uaccess and may block via
// the process-layer restart model (tty reads). Registered into the syscall
// dispatch tables from sys_fs.cpp's own init.

namespace sys_fs
{
    // Fill the file-syscall slots of the dispatch tables. Called by
    // syscall::init().
    void register_handlers();
}

#endif // SYS_FS_H
