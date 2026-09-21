#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

// Device filesystem (stage 3.5): /dev/null, /dev/zero, /dev/tty and
// /dev/console. Read-only namespace (no create/unlink), character devices
// with vnode type CHR. Mounted over the mount point the root FS provides
// for "dev".
//
//   /dev/null     read -> 0 bytes (EOF), write -> swallows
//   /dev/zero     read -> zeroes, write -> swallows
//   /dev/tty      the console terminal (canonical read via tty.cpp,
//                 byte-exact write, TCGETS ioctl)
//   /dev/console  the same terminal
//
// A tty read with no line available returns -EAGAIN; the syscall layer
// blocks on Wait::Key and restarts (poll_ready tells it when to wake).

namespace devfs
{
    extern vfs_fs fs;           // pass to vfs::mount_at("/dev" mount point)

    // Device ids, for stat and diagnostics.
    enum dev_id : uint64_t
    {
        DEV_NULL = 1,
        DEV_ZERO,
        DEV_TTY,
        DEV_CONSOLE,
    };
}

#endif // DEVFS_H
