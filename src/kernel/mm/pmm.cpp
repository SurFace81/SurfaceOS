// Physical Memory Manager: bitmap-based 4 KiB frame allocator.
// Builds the free/used picture from the bootloader memory map and
// reserves regions that the bootloader/kernel use but that the map
// reports as free.

#include "../../include/mm/pmm.h"
#include "../../include/boot/boot.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/screen.h"   // SCREEN_BACKBUFFER_ADDR

// Static kernel-owned regions not covered by the memory map
#define PMM_LOW_RESERVE_END     0x800000ULL   // boot data + kernel + old page tables
#define PMM_HEAP_START          0x2000000ULL
#define PMM_HEAP_SIZE           (9 * 1024 * 1024)

namespace pmm
{
    static uint8_t* bitmap = (uint8_t*)PMM_BITMAP_ADDR;

    static uint64_t bitmap_frames = (PMM_BITMAP_SIZE * 8);  // frames the bitmap can track
    static uint64_t total_frames = 0;
    static uint64_t used_frames  = 0;
    static uint64_t max_phys     = 0;
    static bool     initialized  = false;

    static inline bool bit_get(uint64_t idx)
    {
        return (bitmap[idx / 8] >> (idx % 8)) & 1;
    }

    static inline void bit_set(uint64_t idx)
    {
        bitmap[idx / 8] |= (uint8_t)(1 << (idx % 8));
    }

    static inline void bit_clear(uint64_t idx)
    {
        bitmap[idx / 8] &= (uint8_t)~(1 << (idx % 8));
    }

    static void mark_used(uint64_t frame_idx)
    {
        if (frame_idx < bitmap_frames && !bit_get(frame_idx))
        {
            bit_set(frame_idx);
            if (initialized)
                used_frames++;
        }
    }

    static void mark_free(uint64_t frame_idx)
    {
        if (frame_idx < bitmap_frames && bit_get(frame_idx))
        {
            bit_clear(frame_idx);
            if (initialized)
                used_frames--;
        }
    }

    void reserve(uint64_t start, uint64_t end)
    {
        // Align outward to frame boundaries
        uint64_t first = start / FRAME_SIZE;
        uint64_t last  = (end + FRAME_SIZE - 1) / FRAME_SIZE;

        for (uint64_t i = first; i < last; i++)
            mark_used(i);
    }

    void init(BOOT_HEADER* boot_header)
    {
        // Everything starts as used
        memory::memset(bitmap, 0xFF, PMM_BITMAP_SIZE);
        used_frames = bitmap_frames;
        total_frames = 0;
        max_phys = 0;

        // Free regions reported by the bootloader (type 0)
        MEMORY_MAP_ENTRY* map = (MEMORY_MAP_ENTRY*)boot_header->MemoryMapAddress;
        uint64_t entries = boot_header->MemoryMapEntriesNumber;
        uint32_t entry_size = boot_header->MemoryMapEntrySize;

        for (uint64_t i = 0; i < entries; i++)
        {
            MEMORY_MAP_ENTRY* e = (MEMORY_MAP_ENTRY*)((uint8_t*)map + i * entry_size);

            if (e->End > max_phys)
                max_phys = e->End;

            if (e->Type != 0)
                continue;

            uint64_t first = e->Start / FRAME_SIZE;
            uint64_t last  = e->End / FRAME_SIZE;
            for (uint64_t f = first; f < last; f++)
                mark_free(f);
        }

        // Clamp to bitmap capacity (32 GB)
        if (max_phys > bitmap_frames * FRAME_SIZE)
            max_phys = bitmap_frames * FRAME_SIZE;

        // Regions used by the boot chain that the map reports as free:
        reserve(0, PMM_LOW_RESERVE_END);                                  // boot data, kernel, old page tables
        reserve(PMM_BITMAP_ADDR, PMM_BITMAP_ADDR + PMM_BITMAP_SIZE);      // this bitmap
        reserve(PMM_HEAP_START, PMM_HEAP_START + PMM_HEAP_SIZE);          // kernel heap
        reserve(boot_header->StartDataAddress,
                boot_header->StartDataAddress + boot_header->StartDataSize);
        reserve((uint64_t)boot_header->FrameBufferAddress,
                (uint64_t)boot_header->FrameBufferAddress + boot_header->FrameBufferSize);

        // The screen driver keeps a full-frame back buffer at SCREEN_BACKBUFFER_ADDR.
        // On a 2K display it spans ~15 MB and would otherwise be handed out by
        // the frame allocator (this corrupted page tables on real hardware).
        reserve(SCREEN_BACKBUFFER_ADDR,
                SCREEN_BACKBUFFER_ADDR + boot_header->FrameBufferSize);

        total_frames = max_phys / FRAME_SIZE;

        // Recount used frames within the managed range
        // (the bitmap starts fully set, including frames beyond max_phys)
        used_frames = 0;
        for (uint64_t i = 0; i < total_frames; i++)
        {
            if (bit_get(i))
                used_frames++;
        }

        initialized = true;

        uart::printf("pmm: %llu MB managed, %llu frames free / %llu total\n",
            max_phys / (1024 * 1024), total_frames - used_frames, total_frames);
    }

    uint64_t alloc_frame()
    {
        if (!initialized)
            return 0;

        uint64_t limit = max_phys / FRAME_SIZE;
        for (uint64_t i = 0; i < limit; i++)
        {
            if (!bit_get(i))
            {
                mark_used(i);
                return i * FRAME_SIZE;
            }
        }

        uart::printf("pmm: out of physical memory\n");
        return 0;
    }

    uint64_t alloc_frames(uint64_t n)
    {
        if (!initialized || n == 0)
            return 0;

        uint64_t limit = max_phys / FRAME_SIZE;
        uint64_t run = 0;

        for (uint64_t i = 0; i < limit; i++)
        {
            if (bit_get(i))
            {
                run = 0;
                continue;
            }

            run++;
            if (run == n)
            {
                uint64_t base = (i - n + 1) * FRAME_SIZE;
                for (uint64_t j = i - n + 1; j <= i; j++)
                    mark_used(j);
                return base;
            }
        }

        uart::printf("pmm: cannot allocate %u contiguous frames\n", (uint32_t)n);
        return 0;
    }

    void free_frame(uint64_t phys)
    {
        if (!initialized || phys >= max_phys)
            return;

        mark_free(phys / FRAME_SIZE);
    }

    void free_frames(uint64_t phys, uint64_t n)
    {
        if (!initialized)
            return;

        for (uint64_t i = 0; i < n; i++)
        {
            uint64_t addr = phys + i * FRAME_SIZE;
            if (addr >= max_phys)
                break;
            mark_free(addr / FRAME_SIZE);
        }
    }

    bool is_free(uint64_t phys)
    {
        if (!initialized || phys >= max_phys)
            return false;
        return !bit_get(phys / FRAME_SIZE);
    }

    void get_stats(Stats* out)
    {
        out->total_frames = total_frames;
        out->used_frames  = used_frames;
        out->free_frames  = total_frames > used_frames ? total_frames - used_frames : 0;
        out->max_phys     = max_phys;
    }
} // namespace pmm
