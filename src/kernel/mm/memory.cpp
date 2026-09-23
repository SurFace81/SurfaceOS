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
        heap::init(phys_to_virt(HEAP_START), HEAP_SIZE);
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

    void memcpy(uint8_t* dst, uint8_t* src, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            dst[i] = src[i];
        }
    }

    void memcpy(uint8_t* dst, const uint8_t* src, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            dst[i] = src[i];
        }
    }

    int memcmp(const uint8_t* a, const uint8_t* b, uint64_t size)
    {
        for (uint64_t i = 0; i < size; i++)
        {
            if (a[i] != b[i])
                return (int)a[i] - (int)b[i];
        }
        return 0;
    }
} // namespace memory