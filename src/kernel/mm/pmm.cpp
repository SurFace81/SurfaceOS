// Physical Memory Manager: bitmap-based 4 KiB frame allocator.
//
// Builds the free/used picture from the bootloader memory map and reserves
// the regions the boot chain uses but that the map reports as free.
//
// Two invariants matter:
//   * A frame is only ever handed out if it is below paging::identity_limit().
//     The kernel reaches every frame it allocates (page tables, heap, DMA
//     buffers, the screen back buffer) through the identity map, so an
//     allocation the identity map does not cover is unusable memory.
//   * Allocation starts from a rolling cursor, not from frame 0. The linear
//     rescan cost nothing at 128 MB in QEMU and turned into millions of
//     wasted iterations per call on a machine with real RAM.

#include "../../include/mm/pmm.h"
#include "../../include/boot/boot.h"
#include "../../include/mm/memory.h"
#include "../../include/cpu/paging.h"
#include "../../include/drivers/uart.h"

// Provided by linker.ld; end of the kernel image + .bss.
extern "C" uint8_t __kernel_end[];

#define PMM_HEAP_START          0x2000000ULL
#define PMM_HEAP_SIZE           (9 * 1024 * 1024)

namespace pmm
{
    static uint8_t* bitmap = (uint8_t*)PMM_BITMAP_ADDR;

    static uint64_t bitmap_frames = (PMM_BITMAP_SIZE * 8);  // frames the bitmap can track
    static uint64_t total_frames = 0;
    static uint64_t used_frames  = 0;
    static uint64_t max_phys     = 0;
    static uint64_t cursor       = 0;   // next frame index to try
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

        // Never manage memory the kernel cannot address through the identity
        // map, and never more than the bitmap can describe.
        uint64_t id_limit = paging::identity_limit();
        if (max_phys > id_limit)
            max_phys = id_limit;
        if (max_phys > bitmap_frames * FRAME_SIZE)
            max_phys = bitmap_frames * FRAME_SIZE;

        // Regions used by the boot chain that the map reports as free:
        reserve(0, PMM_LOW_RESERVE_END);                                  // boot data, kernel, page tables
        reserve(PMM_BITMAP_ADDR, PMM_BITMAP_ADDR + PMM_BITMAP_SIZE);      // this bitmap
        reserve(PMM_HEAP_START, PMM_HEAP_START + PMM_HEAP_SIZE);          // initial kernel heap
        reserve(boot_header->StartDataAddress,
                boot_header->StartDataAddress + boot_header->StartDataSize);
        reserve((uint64_t)boot_header->FrameBufferAddress,
                (uint64_t)boot_header->FrameBufferAddress + boot_header->FrameBufferSize);

        // The blanket 0..PMM_LOW_RESERVE_END reserve is supposed to cover the
        // kernel image, but say so explicitly so a kernel that outgrows it
        // fails loudly rather than getting its own .bss handed out as a frame.
        uint64_t kernel_end = (uint64_t)__kernel_end;
        reserve(0x200000, kernel_end);
        if (kernel_end > PMM_LOW_RESERVE_END)
            uart::printf("pmm: WARNING kernel image ends at %llx, past the low reserve\n",
                         kernel_end);

        total_frames = max_phys / FRAME_SIZE;

        // Recount used frames within the managed range
        // (the bitmap starts fully set, including frames beyond max_phys)
        used_frames = 0;
        for (uint64_t i = 0; i < total_frames; i++)
        {
            if (bit_get(i))
                used_frames++;
        }

        cursor = 0;
        initialized = true;

        uart::printf("pmm: %llu MB managed, %llu frames free / %llu total\n",
            max_phys / (1024 * 1024), total_frames - used_frames, total_frames);
    }

    uint64_t alloc_frame()
    {
        if (!initialized)
            return 0;

        uint64_t limit = max_phys / FRAME_SIZE;

        // Two passes: from the cursor to the end, then from 0 to the cursor.
        for (uint64_t pass = 0; pass < 2; pass++)
        {
            uint64_t start = (pass == 0) ? cursor : 0;
            uint64_t stop  = (pass == 0) ? limit  : cursor;

            for (uint64_t i = start; i < stop; i++)
            {
                if (bit_get(i))
                    continue;

                mark_used(i);
                cursor = i + 1;
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

        if (n == 1)
            return alloc_frame();

        uint64_t limit = max_phys / FRAME_SIZE;

        for (uint64_t pass = 0; pass < 2; pass++)
        {
            uint64_t start = (pass == 0) ? cursor : 0;
            uint64_t stop  = (pass == 0) ? limit  : cursor;
            uint64_t run   = 0;

            for (uint64_t i = start; i < stop; i++)
            {
                if (bit_get(i))
                {
                    run = 0;
                    continue;
                }

                run++;
                if (run == n)
                {
                    uint64_t first = i - n + 1;
                    for (uint64_t j = first; j <= i; j++)
                        mark_used(j);
                    cursor = i + 1;
                    return first * FRAME_SIZE;
                }
            }
        }

        uart::printf("pmm: cannot allocate %llu contiguous frames\n", n);
        return 0;
    }

    void free_frame(uint64_t phys)
    {
        if (!initialized || phys >= max_phys)
            return;

        uint64_t idx = phys / FRAME_SIZE;
        mark_free(idx);

        // Reuse freed memory promptly instead of walking to the end first.
        if (idx < cursor)
            cursor = idx;
    }

    void free_frames(uint64_t phys, uint64_t n)
    {
        for (uint64_t i = 0; i < n; i++)
        {
            uint64_t addr = phys + i * FRAME_SIZE;
            if (addr >= max_phys)
                break;
            free_frame(addr);
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
