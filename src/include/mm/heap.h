#ifndef HEAP_H
#define HEAP_H

#include "../cpu/types.h"

void*   kmalloc(size_t size);
void    kfree(void* ptr);

struct HeapStats
{
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t block_count;
    size_t free_block_count;
    size_t used_block_count;
    size_t largest_free_block;
};

namespace heap
{
    void init(void* start, size_t size);
    void get_stats(HeapStats* out);
}

#endif