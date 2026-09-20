// Partition table parsing (stage 3.2). See part.h.
//
// Everything is read through blkdev (one-off boot reads, no cache yet: it is
// initialised after enumeration so it can size its blocks from the devices).
// A disk whose sector 0 is neither GPT nor a FAT-style MBR but is itself a
// FAT32 boot sector is a superfloppy: the whole disk is one volume and gets
// no children.

#include "../../include/dev/part.h"
#include "../../include/dev/blkdev.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    const uint32_t MAX_PARTS_PER_DISK = 8;
    const uint32_t MAX_GPT_ENTRIES    = 128;    // 16 KiB at 128-byte entries

    struct mbr_entry
    {
        uint8_t  boot;
        uint8_t  chs_first[3];
        uint8_t  type;
        uint8_t  chs_last[3];
        uint32_t lba_start;
        uint32_t lba_size;
    } __attribute__((packed));

    struct gpt_header
    {
        uint8_t  signature[8];      // "EFI PART"
        uint32_t revision;
        uint32_t header_size;
        uint32_t header_crc;
        uint32_t reserved;
        uint64_t current_lba;
        uint64_t backup_lba;
        uint64_t first_usable_lba;
        uint64_t last_usable_lba;
        uint8_t  disk_guid[16];
        uint64_t entries_lba;
        uint32_t num_entries;
        uint32_t entry_size;
        uint32_t entries_crc;
    } __attribute__((packed));

    struct gpt_entry
    {
        uint8_t  type_guid[16];
        uint8_t  unique_guid[16];
        uint64_t first_lba;
        uint64_t last_lba;
        uint64_t attrs;
        uint8_t  name_utf16[72];
    } __attribute__((packed));

    uint32_t crc32(const uint8_t* data, uint32_t len)
    {
        uint32_t crc = 0xFFFFFFFF;
        while (len--)
        {
            crc ^= *data++;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xFFFFFFFF;
    }

    uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    uint32_t rd32(const uint8_t* p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    bool mbr_signature(const uint8_t* sector, uint32_t sector_size)
    {
        // The MBR / protective-MBR layout is a fixed 512-byte structure at the
        // start of LBA 0: the 0xAA55 signature sits at offset 510 even on a
        // 4096-byte-sector device (the rest of the sector is unused).
        if (sector_size < 512)
            return false;
        return sector[510] == 0x55 && sector[511] == 0xAA;
    }

    // The four FAT32 MBR types plus the EFI system partition.
    bool mbr_type_is_fat(uint8_t type)
    {
        switch (type)
        {
            case 0x0B: case 0x0C:           // FAT32 CHS / LBA
            case 0x1B: case 0x1C: case 0x1E: // hidden FAT32 variants
            case 0xEF:                       // EFI system partition
                return true;
            default:
                return false;
        }
    }
}

namespace part
{
    bool is_fat_boot_sector(const uint8_t* sector, uint32_t sector_size)
    {
        // FAT32 boot sector: the BPB must agree with the device geometry and
        // have the FAT32-specific fields set (16-bit ones zero). The 0xAA55
        // signature sits at offset 510 even on 4 KiB sectors.
        if (sector_size < 512)
            return false;
        if (sector[0] != 0xEB && sector[0] != 0xE9)
            return false;
        if (sector[510] != 0x55 || sector[511] != 0xAA)
            return false;
        if (rd16(sector + 11) != sector_size)        // bytes_per_sector
            return false;
        uint8_t spc = sector[13];                    // sectors_per_cluster
        if (spc == 0 || spc > 128 || (spc & (spc - 1)))
            return false;
        if (rd16(sector + 14) == 0)                  // reserved_sectors
            return false;
        if (sector[16] == 0)                         // num_fats
            return false;
        if (rd16(sector + 17) != 0)                  // root_entry_count: FAT32 = 0
            return false;
        if (rd16(sector + 19) != 0)                  // total_sectors_16: FAT32 = 0
            return false;
        if (rd16(sector + 22) != 0)                  // fat_size_16: FAT32 = 0
            return false;
        if (rd32(sector + 36) == 0)                  // fat_size_32
            return false;
        return true;
    }

    // -----------------------------------------------------------------------

    static void register_child(blkdev* disk, const char* disk_name, uint32_t index,
                               uint64_t start, uint64_t size)
    {
        // Name: <disk>p<index>, e.g. usb0p1.
        char name[16];
        uint32_t n = 0;
        for (const char* s = disk_name; *s && n < 10; s++)
            name[n++] = *s;
        name[n++] = 'p';
        char digits[8];
        uint32_t d = 0, v = index;
        if (v == 0)
            digits[d++] = '0';
        while (v)
        {
            digits[d++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (d && n < 15)
            name[n++] = digits[--d];
        name[n] = '\0';

        blkdev* part_dev = block::alloc_partition(disk, name, start, size);
        if (!part_dev)
        {
            uart::printf("part: %s: cannot register partition %u\n", disk_name, index);
            return;
        }
        uart::printf("part: %s lba %u..%u (%u MB)\n", name, (uint32_t)start,
                     (uint32_t)(start + size - 1),
                     (uint32_t)(size * disk->sector_size / (1024 * 1024)));
    }

    static void parse_gpt(blkdev* disk, const gpt_header* hdr)
    {
        uint32_t esz = hdr->entry_size;
        uint32_t num = hdr->num_entries;
        if (esz < sizeof(gpt_entry) || num == 0 || num > MAX_GPT_ENTRIES)
        {
            uart::printf("part: %s: GPT entries %u x %u out of range\n",
                         disk->name, num, esz);
            return;
        }

        uint32_t total = num * esz;
        uint32_t sectors = (total + disk->sector_size - 1) / disk->sector_size;

        uint8_t* entries = (uint8_t*)kmalloc((uint64_t)sectors * disk->sector_size);
        if (!entries)
            return;

        // Read the whole entry array in max_sectors_per_io chunks.
        uint32_t off = 0;
        bool read_ok = true;
        for (uint32_t s = 0; s < sectors; s += disk->max_sectors_per_io)
        {
            uint32_t n = disk->max_sectors_per_io;
            if (n > sectors - s)
                n = sectors - s;
            if (block::read(disk, hdr->entries_lba + s, n, entries + off) != 0)
            {
                read_ok = false;
                break;
            }
            off += n * disk->sector_size;
        }
        if (!read_ok)
        {
            uart::printf("part: %s: GPT entries unreadable\n", disk->name);
            kfree(entries);
            return;
        }

        if (crc32(entries, total) != hdr->entries_crc)
        {
            uart::printf("part: %s: GPT entry CRC mismatch\n", disk->name);
            kfree(entries);
            return;
        }

        uint32_t registered = 0;
        static const uint8_t zero16[16] = {0};

        for (uint32_t i = 0; i < num; i++)
        {
            const gpt_entry* e = (const gpt_entry*)(entries + (uint64_t)i * esz);
            if (memory::memcmp(e->type_guid, zero16, 16) == 0)
                continue;   // empty slot

            if (e->last_lba < e->first_lba || e->last_lba >= disk->sector_count)
            {
                uart::printf("part: %s: entry %u out of range\n", disk->name, i + 1);
                continue;
            }

            if (registered >= MAX_PARTS_PER_DISK)
                break;
            register_child(disk, disk->name, i + 1, e->first_lba,
                           e->last_lba - e->first_lba + 1);
            registered++;
        }
        kfree(entries);
    }

    static void parse_mbr(blkdev* disk, const uint8_t* sector)
    {
        uint32_t registered = 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            const mbr_entry* e = (const mbr_entry*)(sector + 446 + i * 16);
            if (e->type == 0)
                continue;
            if (!mbr_type_is_fat(e->type))
            {
                uart::printf("part: %s: entry %u type 0x%x ignored (not FAT)\n",
                             disk->name, i + 1, (uint32_t)e->type);
                continue;
            }
            if (e->lba_size == 0 ||
                (uint64_t)e->lba_start + e->lba_size > disk->sector_count)
            {
                uart::printf("part: %s: entry %u out of range\n", disk->name, i + 1);
                continue;
            }
            register_child(disk, disk->name, i + 1, e->lba_start, e->lba_size);
            registered++;
            if (registered >= MAX_PARTS_PER_DISK)
                break;
        }
    }

    void enumerate()
    {
        for (uint32_t di = 0; block::get(di); di++)
        {
            blkdev* disk = block::get(di);
            if (disk->parent)
                continue;       // whole disks only

            uint32_t ss = disk->sector_size;
            uint8_t* sector0 = (uint8_t*)kmalloc(ss);
            if (!sector0)
                return;

            if (block::read(disk, 0, 1, sector0) != 0)
            {
                uart::printf("part: %s: sector 0 unreadable\n", disk->name);
                kfree(sector0);
                continue;
            }

            if (mbr_signature(sector0, ss))
            {
                const mbr_entry* e0 = (const mbr_entry*)(sector0 + 446);

                // Protective MBR + a valid GPT header at LBA 1?
                if (e0->type == 0xEE && disk->sector_count >= 2)
                {
                    uint8_t* sector1 = (uint8_t*)kmalloc(ss);
                    if (sector1)
                    {
                        bool gpt_ok = false;
                        if (block::read(disk, 1, 1, sector1) == 0)
                        {
                            const gpt_header* hdr = (const gpt_header*)sector1;
                            if (memory::memcmp(hdr->signature, (uint8_t*)"EFI PART", 8) == 0 &&
                                hdr->header_size >= 92 && hdr->header_size <= ss)
                            {
                                // Header CRC over header_size with the CRC
                                // field zeroed.
                                uint8_t* tmp = (uint8_t*)kmalloc(hdr->header_size);
                                if (tmp)
                                {
                                    memory::memcpy(tmp, sector1, hdr->header_size);
                                    memory::memset(tmp + 16, 0, 4);
                                    gpt_ok = crc32(tmp, hdr->header_size) == hdr->header_crc;
                                    kfree(tmp);
                                }
                            }
                        }

                        if (gpt_ok)
                        {
                            uart::printf("part: %s: GPT\n", disk->name);
                            parse_gpt(disk, (const gpt_header*)sector1);
                            kfree(sector1);
                            kfree(sector0);
                            continue;
                        }
                        kfree(sector1);
                        uart::printf("part: %s: 0xEE entry but no valid GPT header\n",
                                     disk->name);
                    }
                }

                // Plain MBR with FAT-type entries?
                bool any_fat = false;
                for (uint32_t i = 0; i < 4; i++)
                {
                    const mbr_entry* e = (const mbr_entry*)(sector0 + 446 + i * 16);
                    if (e->type != 0 && mbr_type_is_fat(e->type))
                        any_fat = true;
                }
                if (any_fat)
                {
                    uart::printf("part: %s: MBR\n", disk->name);
                    parse_mbr(disk, sector0);
                    kfree(sector0);
                    continue;
                }
            }

            if (is_fat_boot_sector(sector0, ss))
            {
                uart::printf("part: %s: superfloppy (FAT32 at LBA 0)\n", disk->name);
            }
            else
            {
                uart::printf("part: %s: no partition table, no FAT boot sector\n",
                             disk->name);
            }
            kfree(sector0);
        }
    }
}
