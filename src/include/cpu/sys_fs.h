#ifndef SYS_FS_H
#define SYS_FS_H

#include "process.h"

struct file;

// File-descriptor syscalls (stage 3.6): Linux numbers, -errno results.
// Every handler validates user pointers through uaccess and may block via
// the process-layer restart model (tty reads). Registered into the syscall
// dispatch tables from sys_fs.cpp's own init.

namespace sys_fs
{
    // Fill the file-syscall slots of the dispatch tables. Called by
    // syscall::init().
    void register_handlers();

    // The cores of openat, read and write, for the SurfaceOS file calls
    // (sffile.cpp). open_at returns the new handle; all return -errno on
    // failure. lflags: vfs LOOKUP_*.
    sint64_t open_at(vnode* base, const char* path, sint32_t flags, uint32_t mode,
                     uint32_t lflags);
    // Open `v` itself (it stays referenced by the caller as well).
    sint64_t open_vnode(vnode* v, sint32_t flags);
    sint64_t read(file* f, uint64_t user_buf, uint64_t count);
    sint64_t write(file* f, uint64_t user_buf, uint64_t count);
}

#endif // SYS_FS_H
