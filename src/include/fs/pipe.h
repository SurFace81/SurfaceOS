#ifndef FS_PIPE_H
#define FS_PIPE_H

#include "../cpu/types.h"

struct vnode;

// A pipe: what one program prints, another reads while both run (the
// console's `a | b`). Two vnodes (vtype::PIPE) share a buffer:
//
//   the write end  a write waits while the buffer is full; -EPIPE once
//                  nobody holds the read end any more;
//   the read end   a read waits while the buffer is empty, then takes
//                  what there is; 0 bytes (the end) once nobody holds the
//                  write end and the buffer is empty.
//
// An end is gone when its vnode's last reference is. Offsets are ignored.
// A wait gives up with -EINTR when the process is to be ended.
namespace pipe
{
    // A new pipe: *rd and *wr its ends, one reference each. 0 or -ENOMEM.
    sint64_t create(vnode** rd, vnode** wr);
}

#endif // FS_PIPE_H
