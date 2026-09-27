#ifndef MOUNTS_H
#define MOUNTS_H

#include "vfs.h"
#include "../dev/blkdev.h"

// Other volumes live under /mount/<device name>: /mount/usb1p1, or
// /mount/usb1 for a disk without a partition table. Mounting makes the
// directory, unmounting removes it again. Used by the console's mount and
// umount and by SfAdmin.
namespace mounts
{
    // The (non-detached) mount of block device `name`, if any.
    mount* of_device(const char* name);

    // Has `disk` partitions of its own (block devices with it as parent)?
    bool has_partitions(blkdev* disk);

    // Mount d on /mount/<d->name>. 0 or -errno.
    sint64_t mount_device(blkdev* d);

    // Flush and unmount m, a mount under /mount, and remove its directory.
    // 0, -EPERM (the root file system), -EBUSY (something has it open) or
    // another -errno.
    sint64_t unmount(mount* m);

    // Before the power goes: flush everything and unmount the root (and
    // with it all below) - only an unmount marks a FAT volume clean.
    // Forced: the machine is going away, open files with it. 0 or -errno.
    sint64_t prepare_power_off();
}

#endif // MOUNTS_H
