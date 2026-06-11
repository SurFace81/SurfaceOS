#ifndef HEAP_H
#define HEAP_H

#include "../cpu/types.h"

void*   kmalloc(size_t size);
void    kfree(void* ptr);

namespace heap
{
    void init(void* start, size_t size);
}

#endif