#include "../../include/cpu/memory.h"

static UINT64 TOT_MEMORY_SIZE;

void setMemorySize(UINT64 size)
{
    TOT_MEMORY_SIZE = size;
}

UINT64 getMemorySize()
{
    return TOT_MEMORY_SIZE;
}

void memset(UINT8 *ptr, char val, UINT64 size)
{
    for (UINT64 i = 0; i < size; i++)
    {
        ptr[i] = val;
    }
}

void memcpy(UINT8 *src, UINT8 *dst, UINT64 size)
{
    for (UINT64 i = 0; i < size; i++)
    {
        dst[i] = src[i];
    }
}

void memalloc(UINT64 src, UINT64 dst, UINT64 size_in_bytes)
{
    UINT64 num_pages = (size_in_bytes + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
    allocate_pages(dst, src, num_pages);
}