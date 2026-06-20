#ifndef MEMORY_H
#define MEMORY_H

#include "../cpu/paging.h"
#include "../cpu/types.h"

namespace memory
{
    void        init(uint64_t mem_size);
    uint64_t    total(void);
    void        memset(uint8_t* addr, char value, uint64_t size);
    void        memcpy(uint8_t* dst, uint8_t* src, uint64_t size);
} // namespace memory

#endif // MEMORY_H