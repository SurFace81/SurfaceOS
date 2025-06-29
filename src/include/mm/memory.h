#ifndef MEMORY_H
#define MEMORY_H

#include "../cpu/types.h"
#include "../cpu/paging.h"
#include "buddy.h"

namespace memory {
    void init(void);
    void setMemorySize(UINT64);
    UINT64 getMemorySize(void);
    bool memfree(UINT8* addr);
    UINT8* memalloc();
    void memset(UINT8* addr, char value, UINT64 size);
    void memcpy(UINT8* src, UINT8* dst, UINT64 size);
    void memalloc(UINT64 src, UINT64 dst, UINT64 size_in_bytes);
}

#endif  // MEMORY_H