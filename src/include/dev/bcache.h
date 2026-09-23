#ifndef BCACHE_H
#define BCACHE_H

#include "../cpu/types.h"
#include "blkdev.h"

// Sector cache (stage 3.2).
//
// Every filesystem read/write goes through here: on real hardware each FAT
// lookup is a separate SCSI round trip, and the old driver paid one per
// sector. The cache is a fixed pool allocated once from the PMM (sized from
// total memory, block size = the largest sector size any registered device
// reports, so 512 and 4096 both work).
//
// Lifecycle of one buffer:
//   bcache::get(dev, lba)      -> locked buf with valid contents (read on
//                                 miss, or zeroed when create=true)
//   bcache::put(buf, dirty)    -> unlock; dirty buffers are written back by
//                                 the next flush()/sync()/eviction.
// Eviction never reuses a locked buffer; if every buffer is locked, get()
// fails with -EBUSY (a caller must not hold more than a couple at a time).
//
// There is no background writeback thread: flushes happen at fsync, close
// of a writable file, umount, process exit and `sync`.

struct buf
{
    uint8_t* data;          // block_size bytes, inside the pool
    blkdev*  dev;
    uint64_t lba;
    bool     valid;
    bool     dirty;
    uint32_t refcnt;
    uint64_t last_use;      // LRU clock
};

namespace bcache
{
    // Call after blkdev/part enumeration so the block size is known.
    void init();

    uint32_t block_size();
    uint32_t capacity();      // number of buffers in the pool

    // Lock (and fill from the device on a miss) the buffer holding dev's
    // sector `lba`. Returns the buffer or nullptr with *out_rc = -errno.
    buf* get(blkdev* dev, uint64_t lba, sint64_t* out_rc);

    // Lock `count` buffers for dev's sectors [lba, lba+count), filling
    // missing ones with as few device requests as possible (contiguous
    // misses are read in one blkdev::read). out[] must hold count slots.
    // Returns 0 and locks every buffer, or -errno with nothing locked.
    sint64_t get_range(blkdev* dev, uint64_t lba, uint32_t count, buf** out);

    // Unlock. dirty=true marks the buffer for writeback.
    void put(buf* b, bool dirty);

    // Write back every dirty buffer of `dev` (nullptr: all devices) and
    // issue the device flush. 0 or -errno.
    sint64_t flush(blkdev* dev);

    // Write back everything, then invalidate every buffer of `dev`
    // (umount path). 0 or -errno.
    sint64_t release(blkdev* dev);

    // Diagnostics for `sync`/meminfo.
    void stats(uint32_t* dirty, uint32_t* used);
}

#endif // BCACHE_H
