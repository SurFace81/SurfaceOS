#ifndef PMM_H
#define PMM_H

#include "../cpu/types.h"
#include "../boot/boot.h"

// Physical frame size managed by PMM.
#define FRAME_SIZE          0x1000ULL

// Fixed low-memory regions the PMM must never hand out. They mirror the
// layout documented in src/kernel/linker.ld:
//
//   0x000000..0x800000   boot data, font, kernel image + .bss, static page
//                        tables. Covered wholesale by PMM_LOW_RESERVE_END.
//   0x2000000..0x2900000 initial kernel heap (see mm/memory.cpp)
//   0x3000000..0x3100000 this bitmap
//
// The screen back buffer used to be a fixed region here too. It is now
// allocated through the PMM (screen::init), because at 4K its 33 MB ran
// straight into the kernel heap.
#define PMM_LOW_RESERVE_END 0x800000ULL
#define PMM_BITMAP_ADDR     0x3000000ULL
#define PMM_BITMAP_SIZE     0x100000ULL     // 1 MB -> covers 32 GB of RAM

namespace pmm
{
    struct Stats
    {
        uint64_t total_frames;
        uint64_t free_frames;
        uint64_t used_frames;
        uint64_t max_phys;      // highest managed physical address (exclusive)
    };

    // Parse bootloader memory map, mark reserved regions, enable allocation.
    void        init(BOOT_HEADER* boot_header);

    // Allocate a single frame, returns physical address (0 on failure).
    uint64_t    alloc_frame();

    // Allocate n contiguous frames, returns base physical address (0 on failure).
    uint64_t    alloc_frames(uint64_t n);

    void        free_frame(uint64_t phys);
    void        free_frames(uint64_t phys, uint64_t n);

    // Mark [start, end) as used (frame-aligned outward).
    void        reserve(uint64_t start, uint64_t end);

    bool        is_free(uint64_t phys);

    void        get_stats(Stats* out);
} // namespace pmm

#endif // PMM_H
