#include "../../include/mm/memory.h"
#include "../../include/mm/heap.h"

static uint64_t total_memory_size;

#define HEAP_START 0x2000000
#define HEAP_SIZE (9 * 1024 * 1024)

namespace memory
{
    void init(uint64_t total_memory)
    {
        total_memory_size = total_memory;
        heap::init((void*)HEAP_START, HEAP_SIZE);
    }

    uint64_t total(void)
    {
        return total_memory_size;
    }

    void memset(uint8_t* ptr, char val, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            ptr[i] = val;
        }
    }

    void memcpy(uint8_t* src, uint8_t* dst, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            dst[i] = src[i];
        }
    }
} // namespace memory