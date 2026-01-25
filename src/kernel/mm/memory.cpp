#include "../../include/mm/memory.h"

static uint64_t TOT_MEMORY_SIZE;

#define ALLOCATOR_START 0x1500000

namespace memory {
    void setMemorySize(uint64_t size)
    {
        TOT_MEMORY_SIZE = size;
    }

    uint64_t getMemorySize(void)
    {
        return TOT_MEMORY_SIZE;
    }

    void init() {
        buddy_init((uint8_t*)ALLOCATOR_START);
    }

    uint8_t* memalloc() {
        uint8_t* ptr = buddy_alloc();
        if (ptr != nullptr)
            return ptr;
        
        return nullptr;
    }

    bool memfree(uint8_t* addr) {
        return buddy_free(addr);
    }

    void memset(uint8_t *ptr, char val, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            ptr[i] = val;
        }
    }

    void memcpy(uint8_t *src, uint8_t *dst, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            dst[i] = src[i];
        }
    }

    void memalloc(uint64_t src, uint64_t dst, uint64_t size_in_bytes)
    {
        uint64_t num_pages = (size_in_bytes + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
        paging::allocate_pages(dst, src, num_pages);
    }
} // namespace