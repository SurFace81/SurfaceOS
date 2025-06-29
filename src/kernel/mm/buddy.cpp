#include "../../include/mm/buddy.h"

static struct {
    UINT8* start_addr;
    UINT64 total_blocks;
    UINT32 bitmap[ALLOCATOR_MAX_BLOCKS / 32];
    UINT64 free_blocks;
} buddy;

namespace memory {
    void buddy_init(UINT8* start_addr) {
        buddy.start_addr    = start_addr;
        buddy.total_blocks  = ALLOCATOR_MAX_BLOCKS;
        buddy.free_blocks   = ALLOCATOR_MAX_BLOCKS;

        for (int i = 0; i < ALLOCATOR_MAX_BLOCKS / 32; i++) {
            buddy.bitmap[i] = 0;
        }
    }

    UINT8* buddy_alloc(void) {
        if (buddy.free_blocks == 0) return nullptr;

        for (UINT64 i = 0; i < buddy.total_blocks; i++) {
            int word = i / 32;
            int bit  = i % 32;

            if (!(buddy.bitmap[word] & (1U << bit))) {
                buddy.bitmap[word] |= (1U << bit);
                buddy.free_blocks  -= 1;
                return (UINT8*)((char*)buddy.start_addr + i * ALLOCATOR_BLOCK_SIZE);
            }
        }

        return nullptr;
    }

    bool buddy_free(UINT8* addr) {
        if (!addr) return false;

        UINT64 index = ((char*)addr - (char*)buddy.start_addr) / ALLOCATOR_BLOCK_SIZE;
        if (index >= buddy.total_blocks) return false;

        int word = index / 32;
        int bit  = index % 32;

        buddy.bitmap[word] &= ~(1U << bit);
        buddy.free_blocks  += 1;

        return true;
    }
}