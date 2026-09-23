#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"

// Copying between kernel and user memory.
//
// Every syscall argument that is a pointer must go through here. Ring 3 got
// added without it, and until then an app could hand any address to the
// time/stat/readdir syscalls and have the kernel write to it at CPL 0 -
// the PMM bitmap, the page tables, the kernel's own code. A null check is
// not a bounds check.
//
// Every page is validated (present, PAGE_USER, writable where needed) before
// a single byte moves, so these helpers never fault and the kernel needs no
// exception fixup table. That is only sound because a syscall cannot be
// preempted by anything that alters the calling process's page tables; when
// a real scheduler arrives this has to become fault-driven instead.
namespace uaccess
{
    // (No MAX_PATH here: PATH_MAX in fs/vfs.h is the single limit. A second,
    // smaller one silently rejected paths the VFS would have accepted.)

    bool copy_from_user(void* dst, uint64_t user_src, uint64_t len);
    bool copy_to_user(uint64_t user_dst, const void* src, uint64_t len);

    // True if [addr, addr+len) is entirely readable (or writable) user memory.
    bool readable(uint64_t addr, uint64_t len);
    bool writable(uint64_t addr, uint64_t len);

    // Copy a NUL-terminated string into `dst`, writing at most `max` bytes
    // including the terminator. `dst` is always NUL-terminated on success.
    //   >= 0  number of bytes before the NUL
    //   -1    the user range is not readable
    //   -2    no NUL found within max-1 bytes (dst holds max-1 bytes + NUL)
    sint64_t strncpy_from_user(char* dst, uint64_t user_src, uint64_t max);
}

#endif // UACCESS_H
