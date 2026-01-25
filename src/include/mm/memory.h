#ifndef MEMORY_H
#define MEMORY_H

#include "../cpu/types.h"
#include "../cpu/paging.h"
#include "buddy.h"

namespace memory {
    void init(void);
    void setMemorySize(uint64_t);
    uint64_t getMemorySize(void);
    bool memfree(uint8_t* addr);
    uint8_t* memalloc();
    void memset(uint8_t* addr, char value, uint64_t size);
    void memcpy(uint8_t* src, uint8_t* dst, uint64_t size);
    void memalloc(uint64_t src, uint64_t dst, uint64_t size_in_bytes);
}

#endif  // MEMORY_H