#ifndef BUDDY_H
#define BUDDY_H

#include "../cpu/types.h"

#define ALLOCATOR_BLOCK_SIZE 1024     // 1Kb
#define ALLOCATOR_MAX_BLOCKS 9216     // ~9Mb

namespace memory {
    void buddy_init(UINT8* start_addr);
    UINT8* buddy_alloc(void);
    bool buddy_free(UINT8* addr);
}

#endif