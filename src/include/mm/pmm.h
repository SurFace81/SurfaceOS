#ifndef PMM_H
#define PMM_H

#include "../cpu/types.h"
#include "../boot/boot.h"

// Physical frame size managed by PMM.
// Note: independent of paging::PAGE_SIZE_BYTES (which may use huge pages).
#define FRAME_SIZE          0x1000ULL

// 1 MB bitmap at 48 MB physical. It MUST sit above every static kernel
// region: boot data/kernel/paging (0..0x800000), kernel heap
// (0x2000000..0x2900000) and any large contiguous PMM allocations the
// heap growth makes. The old location (0x800000) was destroyed by the
// screen back buffer on high-resolution displays, which made the frame
// allocator hand out in-use memory. Covers up to 32 GB of RAM.
#define PMM_BITMAP_ADDR     0x3000000ULL
#define PMM_BITMAP_SIZE     0x100000ULL

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
