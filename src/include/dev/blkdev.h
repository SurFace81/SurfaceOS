#ifndef BLKDEV_H
#define BLKDEV_H

#include "../cpu/types.h"

// Unified block device interface (stage 3.2).
//
// Drivers (today: USB MSD in xhci.cpp) register whole disks; part.cpp
// registers partitions as children of a disk with an lba_offset. Everything
// above (bcache, FAT32, ...) talks only to this interface and never to a
// specific bus.
//
// All calls return 0 on success or a negative errno. Sizes and LBAs are
// 64-bit; the driver enforces its own per-request limit via
// max_sectors_per_io, and block::read/write split larger requests.

struct blkdev;

struct blkdev_ops
{
    // Read `count` sectors starting at `lba` into `buf` (count * sector_size
    // bytes). count <= max_sectors_per_io is guaranteed by block::read.
    sint64_t (*read)(blkdev* dev, uint64_t lba, uint32_t count, void* buf);
    sint64_t (*write)(blkdev* dev, uint64_t lba, uint32_t count, const void* buf);
    // Force the device's write cache to stable storage, if it has one.
    sint64_t (*flush)(blkdev* dev);
};

struct blkdev
{
    char        name[16];           // "usb0", "usb0p1", ...
    uint32_t    sector_size;        // from the device: 512, 4096, ...
    uint64_t    sector_count;
    uint32_t    max_sectors_per_io; // driver limit; blkdev splits above it
    blkdev_ops* ops;
    void*       priv;               // driver-private (e.g. usb device index)

    // Partitions: parent != nullptr, and lba_offset is the partition start
    // on the parent. Whole disks leave both zero/null.
    blkdev*     parent;
    uint64_t    lba_offset;
};

namespace block
{
    // Register whole disks behind every USB mass-storage device: usb0..usbN.
    // Devices READ CAPACITY reports as >= 2 TiB (LBA32 overflow marker) are
    // rejected with an explicit message - READ(16) does not exist yet.
    void enumerate_usb();

    // Register an externally built device (partitions). 0 or -errno.
    sint64_t register_dev(blkdev* dev);

    // Allocate a zeroed blkdev for a partition of `parent` from the static
    // pool. nullptr when the pool is full.
    blkdev* alloc_partition(blkdev* parent, const char* name,
                            uint64_t lba_offset, uint64_t sector_count);

    // Split into max_sectors_per_io chunks, retry failed chunks up to 3
    // times, then -EIO. Partition offsets are applied transparently.
    sint64_t read(blkdev* dev, uint64_t lba, uint32_t count, void* buf);
    sint64_t write(blkdev* dev, uint64_t lba, uint32_t count, const void* buf);
    sint64_t flush(blkdev* dev);

    blkdev*  find(const char* name);
    uint32_t count();
    blkdev*  get(uint32_t index);
}

#endif // BLKDEV_H
