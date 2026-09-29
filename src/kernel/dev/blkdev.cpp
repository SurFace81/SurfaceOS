// Block device layer (stage 3.2).
//
// One registry of blkdev objects; today the only driver behind it is USB MSD
// (drivers/usb/msc.cpp). Partitions (part.cpp) register children of a whole
// disk with an lba_offset; read/write below translate child LBAs into parent
// LBAs and split requests that exceed the driver's per-transfer limit.

#include "../../include/dev/blkdev.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"
#include "../../include/errno.h"

namespace
{
    const uint32_t MAX_BLKDEVS    = 16;     // a few disks + their partitions
    const uint32_t MAX_IO_RETRIES = 3;

    blkdev  devices[MAX_BLKDEVS];
    uint32_t device_count = 0;
}

namespace block
{
    blkdev* register_dev(const blkdev* dev)
    {
        if (!dev || device_count >= MAX_BLKDEVS)
            return nullptr;
        blkdev* reg = &devices[device_count++];
        *reg = *dev;
        uart::printf("blkdev: %s %uB x %u registered\n",
                     reg->name, reg->sector_size, (uint32_t)reg->sector_count);
        return reg;
    }

    blkdev* alloc_partition(blkdev* parent, const char* name,
                            uint64_t lba_offset, uint64_t sector_count)
    {
        if (!parent || device_count >= MAX_BLKDEVS)
            return nullptr;
        if (lba_offset + sector_count > parent->sector_count)
            return nullptr;

        blkdev* dev = &devices[device_count];
        memory::memset((uint8_t*)dev, 0, sizeof(blkdev));
        strncpy(dev->name, name, sizeof(dev->name) - 1);
        dev->sector_size        = parent->sector_size;
        dev->sector_count       = sector_count;
        dev->max_sectors_per_io = parent->max_sectors_per_io;
        dev->ops                = parent->ops;
        dev->priv               = parent->priv;
        dev->parent             = parent;
        dev->lba_offset         = lba_offset;
        device_count++;
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
        blkdev* disk = dev;
        while (disk->parent)
            disk = disk->parent;

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
        for (uint32_t i = 0; i < device_count; i++)
            if (strcmp(devices[i].name, name) == 0)
                return &devices[i];
        return nullptr;
    }

    uint32_t count()
    {
        return device_count;
    }

    blkdev* get(uint32_t index)
    {
        return index < device_count ? &devices[index] : nullptr;
    }
}
