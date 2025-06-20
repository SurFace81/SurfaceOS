#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"
#include "paging.h"

namespace memory {
    void setMemorySize(UINT64);
    UINT64 getMemorySize(void);
    void memset(UINT8* addr, char value, UINT64 size);
    void memcpy(UINT8* src, UINT8* dst, UINT64 size);
    void memalloc(UINT64 src, UINT64 dst, UINT64 size_in_bytes);
}

#endif  // MEMORY_H