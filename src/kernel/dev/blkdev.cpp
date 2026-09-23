// Block device layer (stage 3.2).
//
// One registry of blkdev objects; today the only driver behind it is USB MSD
// (xhci.cpp). Partitions (part.cpp) register children of a whole disk with an
// lba_offset; read/write below translate child LBAs into parent LBAs and
// split requests that exceed the driver's per-transfer limit.

#include "../../include/dev/blkdev.h"
#include "../../include/drivers/usb/xhci.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    const uint32_t MAX_BLKDEVS    = 16;     // a few disks + their partitions
    const uint32_t MAX_IO_RETRIES = 3;

    // 2 TiB with 512-byte sectors: READ(10) carries a 32-bit LBA, so a
    // device reporting last_lba == 0xFFFFFFFF is telling us it needs
    // READ(16), which does not exist yet. Refuse explicitly instead of
    // silently wrapping.
    const uint64_t MAX_LBA32_SECTORS = 0xFFFFFFFFULL;

    blkdev  devices[MAX_BLKDEVS];
    uint32_t device_count = 0;

    // ---- USB MSD glue ------------------------------------------------------

    struct usb_priv
    {
        uint8_t index;
    };
    usb_priv usb_privs[8];

    sint64_t usb_read(blkdev* dev, uint64_t lba, uint32_t count, void* buf)
    {
        usb_priv* p = (usb_priv*)dev->priv;
        // read_sectors caps count at USB_MAX_XFER_SECTORS; block::read
        // never hands us more.
        usb_status st = usb::read_sectors(p->index, (uint32_t)lba,
                                          (uint16_t)count, buf);
        return st == USB_OK ? 0 : -EIO;
    }

    sint64_t usb_write(blkdev* dev, uint64_t lba, uint32_t count, const void* buf)
    {
        usb_priv* p = (usb_priv*)dev->priv;
        usb_status st = usb::write_sectors(p->index, (uint32_t)lba,
                                           (uint16_t)count, buf);
        return st == USB_OK ? 0 : -EIO;
    }

    sint64_t usb_flush(blkdev* dev)
    {
        usb_priv* p = (usb_priv*)dev->priv;
        usb_status st = usb::flush_cache(p->index);
        return st == USB_OK ? 0 : -EIO;
    }

    blkdev_ops usb_ops = { usb_read, usb_write, usb_flush };

    void make_name(char* dst, const char* prefix, uint32_t n)
    {
        uint32_t i = 0;
        for (; prefix[i] && i < 11; i++)
            dst[i] = prefix[i];
        // append the decimal index
        char num[8];
        uint32_t j = 0, v = n;
        if (v == 0)
            num[j++] = '0';
        else
        {
            char tmp[8];
            uint32_t t = 0;
            while (v)
            {
                tmp[t++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (t)
                num[j++] = tmp[--t];
        }
        for (uint32_t k = 0; k < j && i < 14; k++)
            dst[i++] = num[k];
        dst[i] = '\0';
    }
}

namespace block
{
    void enumerate_usb()
    {
        uint8_t n = usb::get_block_device_count();
        for (uint8_t i = 0; i < n && device_count < MAX_BLKDEVS; i++)
        {
            usb_block_device info;
            if (usb::get_block_device_info(i, &info) != USB_OK)
            {
                uart::printf("blkdev: usb msd %u: no capacity, skipped\n", (uint32_t)i);
                continue;
            }
            if (!info.ready)
            {
                uart::printf("blkdev: usb msd %u: not ready, skipped\n", (uint32_t)i);
                continue;
            }

            uint64_t sectors = (uint64_t)info.last_lba + 1;
            if (info.last_lba == MAX_LBA32_SECTORS || sectors > MAX_LBA32_SECTORS)
            {
                // 0xFFFFFFFF is READ CAPACITY(10)'s "too big, use READ(16)"
                // marker; beyond that we simply do not have the commands.
                uart::printf("blkdev: usb msd %u: >= 2 TiB (last_lba=%u) - "
                             "READ(16) not supported yet, skipped\n",
                             (uint32_t)i, info.last_lba);
                continue;
            }
            if (info.block_size == 0 ||
                (info.block_size & (info.block_size - 1)) != 0 ||
                info.block_size < 512 || info.block_size > 4096)
            {
                uart::printf("blkdev: usb msd %u: bogus sector size %u, skipped\n",
                             (uint32_t)i, info.block_size);
                continue;
            }

            if (i >= 8)
                break;

            blkdev* dev = &devices[device_count];
            memory::memset((uint8_t*)dev, 0, sizeof(blkdev));
            make_name(dev->name, "usb", (uint32_t)i);
            dev->sector_size        = info.block_size;
            dev->sector_count       = sectors;
            dev->max_sectors_per_io = USB_MAX_XFER_SECTORS;
            dev->ops                = &usb_ops;
            usb_privs[i].index      = i;
            dev->priv               = &usb_privs[i];
            dev->parent             = nullptr;
            dev->lba_offset         = 0;
            device_count++;

            uart::printf("blkdev: %s %uB x %u (%u MB)\n", dev->name,
                         dev->sector_size, (uint32_t)sectors,
                         (uint32_t)(sectors * dev->sector_size / (1024 * 1024)));
        }
    }

    sint64_t register_dev(blkdev* dev)
    {
        if (!dev || device_count >= MAX_BLKDEVS)
            return -ENOMEM;
        devices[device_count] = *dev;
        device_count++;
        uart::printf("blkdev: %s %uB x %u registered\n",
                     devices[device_count - 1].name,
                     devices[device_count - 1].sector_size,
                     (uint32_t)devices[device_count - 1].sector_count);
        return 0;
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
