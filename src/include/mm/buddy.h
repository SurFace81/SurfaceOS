#ifndef BUDDY_H
#define BUDDY_H

#include "../cpu/types.h"

#define ALLOCATOR_BLOCK_SIZE 1024     // 1Kb
#define ALLOCATOR_MAX_BLOCKS 9216     // ~9Mb

namespace memory {
    void buddy_init(uint8_t* start_addr);
    uint8_t* buddy_alloc(void);
    bool buddy_free(uint8_t* addr);
}

#endif