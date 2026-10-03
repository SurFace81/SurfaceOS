// Sector cache (stage 3.2). See bcache.h for the contract.
//
// The pool comes from the PMM as individual frames (a single 4 MiB
// contiguous allocation would fail on a fragmented machine); the buffer
// descriptors live in one kmalloc array. The block size is the frame: one
// buffer holds one sector of any size a disk may have (512 to 4096), so a
// disk plugged in later fits the pool made at boot.

#include "../../include/dev/bcache.h"
#include "../../include/dev/blkdev.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../include/errno.h"
#include "../../include/drivers/term.h"

namespace
{
    const uint32_t CACHE_MIN_BUFS = 256;    // floor for tiny machines
    const uint32_t CACHE_MAX_BUFS = 2048;   // ceiling: 8 MiB at 4 KiB blocks

    // get_range limits: one call locks at most MAX_RANGE_COUNT buffers; a
    // contiguous miss run is filled with a scratch kmalloc no bigger than
    // MAX_RANGE_CHUNK sectors (64 x 4096 = 256 KiB worst case).
    const uint32_t MAX_RANGE_COUNT = 512;
    const uint32_t MAX_RANGE_CHUNK = 64;

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

    // Where a run of sectors is put together for one write. All of the
    // cache runs under the disks' lock (blkdev.h), so one will do.
    const uint32_t STAGE_BYTES = 64 * 1024;     // a USB transfer at most
    const uint64_t PROGRESS_FROM = 1024 * 1024; // a flush this big shows on the title bar
    uint8_t stage[STAGE_BYTES];

    bool dirty_at(blkdev* dev, uint64_t lba, buf** out)
    {
        buf* b = find_buffer(dev, lba);
        *out = b;
        return b && b->dirty;
    }

    // Write a buffer out, and with it the dirty buffers of the sectors
    // around it, as one request: a sector at a time, a big file took a
    // command for every 512 bytes. 0 or -EIO. The buffers stay valid: a
    // failed write must not silently drop data, the caller decides what
    // to do. Returns how many sectors went in *count (may be null).
    sint64_t writeback(buf* b, uint32_t* count = nullptr)
    {
        if (count)
            *count = 0;
        if (!b->dirty)
            return 0;
        // Its disk was unplugged: the data cannot be written anywhere.
        // It is dropped, so it does not fail every later sync as well.
        if (block::gone(b->dev))
        {
            b->dirty = false;
            return -EIO;
        }

        blkdev* dev = b->dev;
        uint32_t ss = dev->sector_size;
        uint32_t max = ss && ss <= STAGE_BYTES ? STAGE_BYTES / ss : 1;

        // The run: back to its first dirty sector, then forward.
        buf* first = b;
        buf* p;
        for (uint32_t n = 1; n < max && first->lba && dirty_at(dev, first->lba - 1, &p); n++)
            first = p;
        buf* run[STAGE_BYTES / 512];
        uint32_t n = 0;
        run[n++] = first;
        while (n < max && dirty_at(dev, first->lba + n, &p))
            run[n++] = p;

        sint64_t rc;
        if (n == 1)
            rc = block::write(dev, first->lba, 1, first->data);
        else
        {
            for (uint32_t i = 0; i < n; i++)
                memory::memcpy(stage + (uint64_t)i * ss, run[i]->data, ss);
            rc = block::write(dev, first->lba, n, stage);
        }
        if (rc == 0)
            for (uint32_t i = 0; i < n; i++)
                run[i]->dirty = false;
        if (count)
            *count = n;
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

        block_sz = (uint32_t)FRAME_SIZE;

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
            buffers[i].data = (uint8_t*)phys_to_virt(frame);
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

        if (b->valid && b->dirty && !block::gone(b->dev))
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

    // Fill a contiguous run of cache-miss sectors with one device request.
    // `bufs[]` are already reserved (valid, refcnt==1) but hold stale data;
    // they cover [lba, lba+run).
    sint64_t fill_run(blkdev* dev, uint64_t lba, buf** bufs, uint32_t run)
    {
        // Read into a scratch buffer then scatter, so we do not need the run
        // to be physically contiguous in the pool.
        uint32_t ss = dev->sector_size;
        uint32_t chunk = run;
        if (chunk > MAX_RANGE_CHUNK)
            chunk = MAX_RANGE_CHUNK;

        uint8_t* scratch = (uint8_t*)kmalloc((uint64_t)chunk * ss);
        if (!scratch)
            return -ENOMEM;

        uint32_t done = 0;
        sint64_t rc = 0;
        while (done < run)
        {
            uint32_t n = run - done;
            if (n > chunk)
                n = chunk;

            rc = block::read(dev, lba + done, n, scratch);
            if (rc != 0)
                break;

            for (uint32_t i = 0; i < n; i++)
            {
                buf* b = bufs[done + i];
                memory::memcpy(b->data, scratch + (uint64_t)i * ss, ss);
                if (block_sz > ss)
                    memory::memset(b->data + ss, 0, block_sz - ss);
            }
            done += n;
        }

        kfree(scratch);
        return rc;
    }

    sint64_t get_range(blkdev* dev, uint64_t lba, uint32_t count, buf** out)
    {
        if (!inited || !dev)
            return -ENXIO;
        if (count == 0)
            return 0;
        if (count > MAX_RANGE_COUNT)
            return -EINVAL;
        if (lba + count > dev->sector_count)
            return -EINVAL;

        uint8_t* miss = (uint8_t*)kmalloc(count);
        if (!miss)
            return -ENOMEM;

        // Phase 1: lock every buffer, remembering which were cache misses.
        for (uint32_t i = 0; i < count; i++)
        {
            miss[i] = 0;

            buf* b = find_buffer(dev, lba + i);
            if (b)
            {
                b->refcnt++;
                b->last_use = ++clock;
                out[i] = b;
                continue;
            }

            b = pick_victim();
            if (!b)
            {
                for (uint32_t k = 0; k < i; k++)
                    put(out[k], false);
                kfree(miss);
                return -EBUSY;
            }
            if (b->valid && b->dirty)
            {
                sint64_t rc = writeback(b);
                if (rc != 0)
                {
                    for (uint32_t k = 0; k < i; k++)
                        put(out[k], false);
                    kfree(miss);
                    return rc;
                }
            }
            b->dev      = dev;
            b->lba      = lba + i;
            b->valid    = true;
            b->dirty    = false;
            b->refcnt   = 1;
            b->last_use = ++clock;
            out[i] = b;
            miss[i] = 1;
        }

        // Phase 2: fill each contiguous miss run with as few device requests
        // as the caller's window allows.
        uint32_t i = 0;
        sint64_t rc = 0;
        while (i < count)
        {
            if (!miss[i])
            {
                i++;
                continue;
            }
            uint32_t run = 1;
            while (i + run < count && miss[i + run])
                run++;

            buf* run_bufs[MAX_RANGE_CHUNK];
            uint32_t off = 0;
            while (off < run)
            {
                uint32_t n = run - off;
                if (n > MAX_RANGE_CHUNK)
                    n = MAX_RANGE_CHUNK;
                for (uint32_t k = 0; k < n; k++)
                    run_bufs[k] = out[i + off + k];

                rc = fill_run(dev, lba + i + off, run_bufs, n);
                if (rc != 0)
                    break;
                off += n;
            }
            if (rc != 0)
                break;
            i += run;
        }

        kfree(miss);

        if (rc != 0)
        {
            for (uint32_t k = 0; k < count; k++)
                put(out[k], false);
            return rc;
        }
        return 0;
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

        // A long one says how far it is, on the title bar.
        uint64_t total = 0, written = 0;
        for (uint32_t i = 0; i < nbuf; i++)
            if (buffers[i].valid && buffers[i].dirty && (!dev || buffers[i].dev == dev))
                total += buffers[i].dev->sector_size;
        bool show = total >= PROGRESS_FROM;

        sint64_t first_err = 0;
        for (uint32_t i = 0; i < nbuf; i++)
        {
            buf* b = &buffers[i];
            if (!b->valid || !b->dirty)
                continue;
            if (dev && b->dev != dev)
                continue;

            // A buffer of an unplugged disk fails only a flush of that
            // disk, not a flush of everything.
            bool lost = block::gone(b->dev);
            uint32_t ss = b->dev->sector_size;
            uint32_t n = 0;
            sint64_t rc = writeback(b, &n);
            if (rc != 0 && first_err == 0 && (dev || !lost))
                first_err = rc;
            written += (uint64_t)n * ss;
            if (show)
                term::set_progress("disk", written < total ? written : total, total);
        }
        if (show)
            term::set_status("", 0);

        // The device's own write cache (if any) must reach stable storage
        // before we claim "synced".
        for (uint32_t i = 0; block::get(i); i++)
        {
            blkdev* d = block::get(i);
            if (d->parent)
                continue;               // one SYNCHRONIZE CACHE per disk
            if (d->gone && !dev)
                continue;               // unplugged: nothing to flush into
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

    void discard(blkdev* dev)
    {
        for (uint32_t i = 0; i < nbuf; i++)
        {
            buf* b = &buffers[i];
            if (!b->valid || b->dev != dev)
                continue;
            b->dirty = false;
            // A locked buffer stays with its holder until put(); the
            // device is not dropped while anything holds it.
            if (b->refcnt == 0)
            {
                b->valid = false;
                b->dev   = nullptr;
            }
        }
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
