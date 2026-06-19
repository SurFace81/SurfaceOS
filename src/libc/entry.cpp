#include "./include/heap.h"
#include "./include/stdio.h"

struct program_info
{
    uint64_t heap_start;
    uint64_t heap_size;
};

extern int main();

extern "C" void _start(program_info* info)
{
    heap_init((void*)info->heap_start, info->heap_size);
    main();
    exit(0);
}