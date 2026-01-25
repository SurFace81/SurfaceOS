#include "../../include/mm/buddy.h"

static struct {
    uint8_t* start_addr;
    uint64_t total_blocks;
    uint32_t bitmap[ALLOCATOR_MAX_BLOCKS / 32];
    uint64_t free_blocks;
} buddy;

namespace memory {
    void buddy_init(uint8_t* start_addr) {
        buddy.start_addr    = start_addr;
        buddy.total_blocks  = ALLOCATOR_MAX_BLOCKS;
        buddy.free_blocks   = ALLOCATOR_MAX_BLOCKS;

        for (int i = 0; i < ALLOCATOR_MAX_BLOCKS / 32; i++) {
            buddy.bitmap[i] = 0;
        }
    }

    uint8_t* buddy_alloc(void) {
        if (buddy.free_blocks == 0) return nullptr;

        for (uint64_t i = 0; i < buddy.total_blocks; i++) {
            int word = i / 32;
            int bit  = i % 32;

            if (!(buddy.bitmap[word] & (1U << bit))) {
                buddy.bitmap[word] |= (1U << bit);
                buddy.free_blocks  -= 1;
                return (uint8_t*)((char*)buddy.start_addr + i * ALLOCATOR_BLOCK_SIZE);
            }
        }

        return nullptr;
    }

    bool buddy_free(uint8_t* addr) {
        if (!addr) return false;

        uint64_t index = ((char*)addr - (char*)buddy.start_addr) / ALLOCATOR_BLOCK_SIZE;
        if (index >= buddy.total_blocks) return false;

        int word = index / 32;
        int bit  = index % 32;

        buddy.bitmap[word] &= ~(1U << bit);
        buddy.free_blocks  += 1;

        return true;
    }
}