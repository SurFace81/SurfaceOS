#ifndef PART_H
#define PART_H

#include "../cpu/types.h"
#include "blkdev.h"

// Partition table parsing (stage 3.2).
//
// Sector 0 of every whole disk is classified as one of:
//   - GPT:        protective MBR (single 0xEE entry) + "EFI PART" at LBA 1
//   - MBR:        valid partition entries; FAT-ish types (0x0B/0x0C/0x0E/
//                 0x1B/0x1C/0x1E and 0xEF) become children usbNpM
//   - superfloppy: sector 0 is a FAT32 BPB (jump boot + sane geometry);
//                 the whole disk is one volume, no children
//
// Children are registered through block::alloc_partition and appear as
// usb0p1... in lsblk.

namespace part
{
    // Scan every whole disk (parent == nullptr) and register partitions.
    void enumerate();

    // True if sector 0 of `dev` looks like a FAT32 boot sector (superfloppy).
    bool is_fat_boot_sector(const uint8_t* sector, uint32_t sector_size);
}

#endif // PART_H
