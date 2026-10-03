// Block device layer (stage 3.2).
//
// One registry of blkdev objects; today the only driver behind it is USB MSD
// (drivers/usb/msc.cpp). Partitions (part.cpp) register children of a whole
// disk with an lba_offset; read/write below translate child LBAs into parent
// LBAs and split requests that exceed the driver's per-transfer limit.

#include "../../include/dev/blkdev.h"
#include "../../include/dev/bcache.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/screen.h"
#include "../../include/stdlib/string.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"
#include "../../include/errno.h"
#include "../../include/cpu/wait.h"

static sleep_lock disks_lock;

block::guard::guard()  : taken(wait::lock(&disks_lock)) {}
block::guard::~guard() { if (taken) wait::unlock(&disks_lock); }
void block::pass_turn()  { wait::pass(&disks_lock); }

namespace
{
    const uint32_t MAX_BLKDEVS    = 16;     // a few disks + their partitions
    const uint32_t MAX_IO_RETRIES = 3;

    // Entries are reused once their device left the registry; pointers to
    // live entries stay valid.
    blkdev  devices[MAX_BLKDEVS];

    blkdev* free_entry()
    {
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
            if (!devices[i].registered)
                return &devices[i];
        return nullptr;
    }

    blkdev* disk_of(blkdev* dev)
    {
        while (dev->parent)
            dev = dev->parent;
        return dev;
    }

    // A gone disk that nothing holds any more - neither it nor one of its
    // partitions - leaves the registry, partitions first.
    void collect(blkdev* disk)
    {
        if (!disk->gone || disk->users)
            return;
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
            if (devices[i].registered && devices[i].parent == disk && devices[i].users)
                return;

        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
        {
            blkdev* d = &devices[i];
            if (d->registered && d->parent == disk)
            {
                bcache::discard(d);
                memory::memset((uint8_t*)d, 0, sizeof(blkdev));
            }
        }
        bcache::discard(disk);
        uart::printf("blkdev: %s removed\n", disk->name);
        if (disk->ops && disk->ops->forget)
            disk->ops->forget(disk);
        memory::memset((uint8_t*)disk, 0, sizeof(blkdev));
    }
}

namespace block
{
    blkdev* register_dev(const blkdev* dev)
    {
        blkdev* reg = dev ? free_entry() : nullptr;
        if (!reg)
            return nullptr;
        *reg = *dev;
        reg->registered = true;
        reg->gone = false;
        reg->users = 0;
        uart::printf("blkdev: %s %uB x %u registered\n",
                     reg->name, reg->sector_size, (uint32_t)reg->sector_count);
        return reg;
    }

    blkdev* alloc_partition(blkdev* parent, const char* name,
                            uint64_t lba_offset, uint64_t sector_count)
    {
        blkdev* dev = parent ? free_entry() : nullptr;
        if (!dev)
            return nullptr;
        if (lba_offset + sector_count > parent->sector_count)
            return nullptr;

        memory::memset((uint8_t*)dev, 0, sizeof(blkdev));
        strncpy(dev->name, name, sizeof(dev->name) - 1);
        dev->sector_size        = parent->sector_size;
        dev->sector_count       = sector_count;
        dev->max_sectors_per_io = parent->max_sectors_per_io;
        dev->ops                = parent->ops;
        dev->priv               = parent->priv;
        dev->parent             = parent;
        dev->lba_offset         = lba_offset;
        dev->registered         = true;
        return dev;
    }

    // Walk to the whole disk this device ultimately sits on and translate the
    // LBA. Partitions nest at most one level today, but the loop costs
    // nothing and survives a future mapper device.
    static blkdev* resolve(blkdev* dev, uint64_t* lba)
    {
        while (dev->parent)
        {
            *lba += dev->lba_offset;
            dev = dev->parent;
        }
        return dev;
    }

    sint64_t read(blkdev* dev, uint64_t lba, uint32_t count, void* buf)
    {
        if (!dev || !dev->ops || count == 0)
            return -EINVAL;
        if (lba + count > dev->sector_count)
            return -EINVAL;     // beyond the device/partition end

        blkdev* real = resolve(dev, &lba);
        if (real->gone)
            return -EIO;
        uint8_t* dst = (uint8_t*)buf;

        uint32_t done = 0;
        while (done < count)
        {
            uint32_t chunk = count - done;
            if (chunk > real->max_sectors_per_io)
                chunk = real->max_sectors_per_io;

            sint64_t rc = -EIO;
            for (uint32_t attempt = 0; attempt < MAX_IO_RETRIES; attempt++)
            {
                rc = real->ops->read(real, lba + done, chunk, dst);
                if (rc == 0)
                    break;
            }
            if (rc != 0)
                return -EIO;

            dst  += (uint64_t)chunk * real->sector_size;
            done += chunk;
        }
        return 0;
    }

    sint64_t write(blkdev* dev, uint64_t lba, uint32_t count, const void* buf)
    {
        if (!dev || !dev->ops || count == 0)
            return -EINVAL;
        if (lba + count > dev->sector_count)
            return -EINVAL;

        blkdev* real = resolve(dev, &lba);
        if (real->gone)
            return -EIO;
        const uint8_t* src = (const uint8_t*)buf;

        uint32_t done = 0;
        while (done < count)
        {
            uint32_t chunk = count - done;
            if (chunk > real->max_sectors_per_io)
                chunk = real->max_sectors_per_io;

            sint64_t rc = -EIO;
            for (uint32_t attempt = 0; attempt < MAX_IO_RETRIES; attempt++)
            {
                rc = real->ops->write(real, lba + done, chunk, src);
                if (rc == 0)
                    break;
            }
            if (rc != 0)
                return -EIO;

            src  += (uint64_t)chunk * real->sector_size;
            done += chunk;
        }
        return 0;
    }

    sint64_t flush(blkdev* dev)
    {
        if (!dev || !dev->ops)
            return -EINVAL;
        if (!dev->ops->flush)
            return 0;           // device without a cache: nothing to do

        // SYNCHRONIZE CACHE is whole-device: send it to the disk itself,
        // not to a partition view of it.
        blkdev* disk = disk_of(dev);
        if (disk->gone)
            return -EIO;

        sint64_t rc = -EIO;
        for (uint32_t attempt = 0; attempt < MAX_IO_RETRIES; attempt++)
        {
            rc = disk->ops->flush(disk);
            if (rc == 0)
                break;
        }
        return rc;
    }

    blkdev* find(const char* name)
    {
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
            if (devices[i].registered && strcmp(devices[i].name, name) == 0)
                return &devices[i];
        return nullptr;
    }

    uint32_t count()
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
            if (devices[i].registered)
                n++;
        return n;
    }

    blkdev* get(uint32_t index)
    {
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
            if (devices[i].registered && index-- == 0)
                return &devices[i];
        return nullptr;
    }

    void disk_gone(blkdev* disk)
    {
        disk->gone = true;

        bool used = disk->users > 0;
        bcache::discard(disk);
        for (uint32_t i = 0; i < MAX_BLKDEVS; i++)
        {
            blkdev* d = &devices[i];
            if (d->registered && d->parent == disk)
            {
                bcache::discard(d);
                if (d->users)
                    used = true;
            }
        }

        if (used)
        {
            // What was not written yet is lost; the user has to know.
            uart::printf("blkdev: %s removed while mounted\n", disk->name);
            screen::printf("\n\r%s was removed while mounted: unmount it (mount lists where)",
                           disk->name);
        }
        collect(disk);
    }

    bool gone(blkdev* dev)
    {
        return disk_of(dev)->gone;
    }

    void hold(blkdev* dev)
    {
        dev->users++;
    }

    void drop(blkdev* dev)
    {
        if (dev->users)
            dev->users--;
        collect(disk_of(dev));
    }
}
