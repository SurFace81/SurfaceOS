// Sector cache (stage 3.2). See bcache.h for the contract.
//
// The pool comes from the PMM as individual frames (a single 4 MiB
// contiguous allocation would fail on a fragmented machine); the buffer
// descriptors live in one kmalloc array. The block size is the largest
// sector size among the registered devices - mixing 512 and 4096 devices
// with one cache is exactly why the size is per-cache, not per-buffer.

#include "../../include/dev/bcache.h"
#include "../../include/dev/blkdev.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    const uint32_t CACHE_MIN_BUFS = 256;    // floor for tiny machines
    const uint32_t CACHE_MAX_BUFS = 2048;   // ceiling: 8 MiB at 4 KiB blocks

    buf*     buffers    = nullptr;
    uint32_t nbuf       = 0;
    uint32_t block_sz   = 0;
    uint64_t clock      = 0;
    bool     inited     = false;

    buf* find_buffer(blkdev* dev, uint64_t lba)
    {
        for (uint32_t i = 0; i < nbuf; i++)
            if (buffers[i].valid && buffers[i].dev == dev && buffers[i].lba == lba)
                return &buffers[i];
        return nullptr;
    }

    // Write one buffer out. 0 or -EIO. The buffer stays valid: a failed
    // write must not silently drop data, the caller decides what to do.
    sint64_t writeback(buf* b)
    {
        if (!b->dirty)
            return 0;
        sint64_t rc = block::write(b->dev, b->lba, 1, b->data);
        if (rc == 0)
            b->dirty = false;
        return rc;
    }

    // Pick a victim: an unlocked buffer, preferring clean, least recently
    // used. Locked buffers are never touched.
    buf* pick_victim()
    {
        buf* best_clean = nullptr;
        buf* best_dirty = nullptr;

        for (uint32_t i = 0; i < nbuf; i++)
        {
            buf* b = &buffers[i];
            if (b->refcnt != 0)
                continue;
            if (!b->valid)
                return b;               // free slot wins outright

            if (b->dirty)
            {
                if (!best_dirty || b->last_use < best_dirty->last_use)
                    best_dirty = b;
            }
            else
            {
                if (!best_clean || b->last_use < best_clean->last_use)
                    best_clean = b;
            }
        }
        return best_clean ? best_clean : best_dirty;
    }
}

namespace bcache
{
    void init()
    {
        if (inited)
            return;

        // Block size: the largest sector size among registered devices, so
        // one pool serves 512-byte and 4096-byte media at the same time.
        block_sz = 512;
        for (uint32_t i = 0; block::get(i); i++)
        {
            uint32_t ss = block::get(i)->sector_size;
            if (ss > block_sz)
                block_sz = ss;
        }

        // Size the pool from installed RAM: 2 MiB of cache per 128 MiB,
        // clamped to [CACHE_MIN_BUFS, CACHE_MAX_BUFS]. The spec forbids
        // assuming a memory size: QEMU runs with 128 MB, a real machine may
        // have more, and the cache must stay a small fraction either way.
        uint64_t total = memory::total();
        uint64_t want_bytes = total / 64;
        uint64_t want_bufs = want_bytes / block_sz;
        nbuf = (uint32_t)want_bufs;
        if (nbuf < CACHE_MIN_BUFS)
            nbuf = CACHE_MIN_BUFS;
        if (nbuf > CACHE_MAX_BUFS)
            nbuf = CACHE_MAX_BUFS;

        buffers = (buf*)kmalloc((uint64_t)nbuf * sizeof(buf));
        if (!buffers)
        {
            nbuf = CACHE_MIN_BUFS;
            buffers = (buf*)kmalloc((uint64_t)nbuf * sizeof(buf));
            if (!buffers)
            {
                uart::printf("bcache: no memory for descriptors, cache off\n");
                nbuf = 0;
                return;
            }
        }
        memory::memset((uint8_t*)buffers, 0, (uint64_t)nbuf * sizeof(buf));

        uint32_t allocated = 0;
        for (uint32_t i = 0; i < nbuf; i++)
        {
            uint64_t frame = pmm::alloc_frame();
            if (!frame)
                break;
            buffers[i].data = (uint8_t*)frame;    // identity-mapped
            allocated++;
        }
        nbuf = allocated;

        if (nbuf < CACHE_MIN_BUFS)
            uart::printf("bcache: warning: only %u buffers (%u KiB)\n",
                         nbuf, nbuf * block_sz / 1024);
        else
            uart::printf("bcache: %u buffers x %uB (%u KiB)\n",
                         nbuf, block_sz, nbuf * block_sz / 1024);

        inited = true;
    }

    uint32_t block_size()
    {
        return block_sz;
    }

    uint32_t capacity()
    {
        return nbuf;
    }

    buf* get(blkdev* dev, uint64_t lba, sint64_t* out_rc)
    {
        if (!inited || !dev)
        {
            if (out_rc) *out_rc = -ENXIO;
            return nullptr;
        }
        if (lba >= dev->sector_count)
        {
            if (out_rc) *out_rc = -EINVAL;
            return nullptr;
        }

        // The kernel is never preempted inside its own code (switches happen
        // at the ring-3 boundary only), so no locking beyond refcnt is
        // needed; refcnt still guards against self-eviction bugs.
        buf* b = find_buffer(dev, lba);
        if (b)
        {
            b->refcnt++;
            b->last_use = ++clock;
            if (out_rc) *out_rc = 0;
            return b;
        }

        b = pick_victim();
        if (!b)
        {
            if (out_rc) *out_rc = -EBUSY;
            return nullptr;
        }

        if (b->valid && b->dirty)
        {
            sint64_t rc = writeback(b);
            if (rc != 0)
            {
                uart::printf("bcache: writeback of %s lba %u failed (%d)\n",
                             b->dev->name, (uint32_t)b->lba, (int)rc);
                if (out_rc) *out_rc = rc;
                return nullptr;
            }
        }

        // Rebind the victim slot, then fill it from the device.
        b->dev      = dev;
        b->lba      = lba;
        b->valid    = true;
        b->dirty    = false;
        b->refcnt   = 1;
        b->last_use = ++clock;

        if (block_sz > dev->sector_size)
            memory::memset(b->data + dev->sector_size, 0, block_sz - dev->sector_size);

        sint64_t rc = block::read(dev, lba, 1, b->data);
        if (rc != 0)
        {
            b->valid = false;
            b->refcnt = 0;
            if (out_rc) *out_rc = rc;
            return nullptr;
        }

        if (out_rc) *out_rc = 0;
        return b;
    }

    void put(buf* b, bool dirty)
    {
        if (!b)
            return;
        if (dirty)
            b->dirty = true;
        if (b->refcnt)
            b->refcnt--;
        b->last_use = ++clock;
    }

    sint64_t flush(blkdev* dev)
    {
        if (!inited)
            return -ENXIO;

        sint64_t first_err = 0;
        for (uint32_t i = 0; i < nbuf; i++)
        {
            buf* b = &buffers[i];
            if (!b->valid || !b->dirty)
                continue;
            if (dev && b->dev != dev)
                continue;

            sint64_t rc = writeback(b);
            if (rc != 0 && first_err == 0)
                first_err = rc;
        }

        // The device's own write cache (if any) must reach stable storage
        // before we claim "synced".
        for (uint32_t i = 0; block::get(i); i++)
        {
            blkdev* d = block::get(i);
            if (d->parent)
                continue;               // one SYNCHRONIZE CACHE per disk
            if (dev)
            {
                blkdev* root = dev;
                while (root->parent)
                    root = root->parent;
                if (d != root)
                    continue;
            }
            sint64_t rc = block::flush(d);
            if (rc != 0 && first_err == 0)
                first_err = rc;
        }

        return first_err;
    }

    sint64_t release(blkdev* dev)
    {
        if (!inited)
            return -ENXIO;

        sint64_t rc = flush(dev);

        for (uint32_t i = 0; i < nbuf; i++)
        {
            buf* b = &buffers[i];
            if (!b->valid)
                continue;
            if (dev && b->dev != dev)
                continue;
            if (b->refcnt != 0)
            {
                uart::printf("bcache: release with %u locked buffers\n", b->refcnt);
                if (rc == 0)
                    rc = -EBUSY;
                continue;
            }
            b->valid = false;
            b->dirty = false;
            b->dev   = nullptr;
        }
        return rc;
    }

    void stats(uint32_t* dirty, uint32_t* used)
    {
        uint32_t d = 0, u = 0;
        for (uint32_t i = 0; i < nbuf; i++)
        {
            if (buffers[i].valid)
                u++;
            if (buffers[i].valid && buffers[i].dirty)
                d++;
        }
        if (dirty) *dirty = d;
        if (used)  *used  = u;
    }
}
