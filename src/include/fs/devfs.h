#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

// Device filesystem (stage 3.5): null, zero, tty and console. Read-only
// namespace (no create/unlink), character devices with vnode type CHR.
// Mounted detached: there is no /dev, the kernel takes the devices it
// needs through devfs::open (the tty for a process's fds 0-2).
//
//   null     read -> 0 bytes (EOF), write -> swallows
//   zero     read -> zeroes, write -> swallows
//   tty      the console terminal (canonical read via tty.cpp,
//            byte-exact write, TCGETS ioctl)
//   console  the same terminal
//
// A tty read with no line available returns -EAGAIN; the syscall layer
// blocks on Wait::Key and restarts (poll_ready tells it when to wake).

namespace devfs
{
    extern vfs_fs fs;

    // Mount the detached instance; call once after vfs::init.
    sint64_t init();

    // Referenced vnode of device `name` ("tty", ...).
    sint64_t open(const char* name, vnode** out);

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
