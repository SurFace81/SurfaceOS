#ifndef FS_FILEIO_H
#define FS_FILEIO_H

#include "../cpu/types.h"

struct vnode;
struct file;

// Files opened, read and written for a program (sffile.cpp). open_at opens
// `path` below `base` into the caller's handle table and returns the new
// handle; all return -errno on failure. lflags: vfs LOOKUP_*.
namespace fileio
{
    sint64_t open_at(vnode* base, const char* path, sint32_t flags, uint32_t mode,
                     uint32_t lflags);
    // Open `v` itself (it stays referenced by the caller as well).
    sint64_t open_vnode(vnode* v, sint32_t flags);
    sint64_t read(file* f, uint64_t user_buf, uint64_t count);
    sint64_t write(file* f, uint64_t user_buf, uint64_t count);
}

#endif // FS_FILEIO_H
