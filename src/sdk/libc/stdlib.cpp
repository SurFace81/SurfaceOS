#include "../include/stdlib.h"
#include "../include/abi/syscall.h"

extern uint64_t syscall(uint64_t num, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);
extern uint64_t brk(uint64_t new_brk);

struct BlockHeader
{
    size_t       size;
    bool         free;
    BlockHeader* next;
};

static BlockHeader* heap_start = (BlockHeader*)NULL;
static uint64_t     heap_brk   = 0;   // current program break (end of mapped heap)
static const size_t HEADER_SIZE = sizeof(BlockHeader);
static const size_t MIN_BLOCK_DATA = 16;

// Minimum number of bytes to request from the kernel per grow
static const size_t GROW_MIN = 64 * 1024;

static size_t align8(size_t size)
{
    return (size + 7) & ~(size_t)7;
}

// Ask the kernel to extend the program break by at least `need` bytes and
// append the new region as a free block. Returns false if the kernel
// cannot satisfy the request.
static bool heap_grow(size_t need)
{
    if (need < GROW_MIN)
        need = GROW_MIN;
    need = align8(need);

    uint64_t old_brk = brk(0);
    uint64_t new_brk = brk(old_brk + need);

    if (new_brk < old_brk + need)
        return false;

    uint64_t got = new_brk - old_brk;
    if (got < HEADER_SIZE + MIN_BLOCK_DATA)
        return false;

    BlockHeader* block = (BlockHeader*)old_brk;
    block->size = got - HEADER_SIZE;
    block->free = true;
    block->next = (BlockHeader*)NULL;

    if (!heap_start)
    {
        heap_start = block;
    }
    else
    {
        // Append to the end of the block list. If physically adjacent to
        // the last block and it is free, merge.
        BlockHeader* last = heap_start;
        while (last->next)
            last = last->next;

        uint8_t* last_end = (uint8_t*)(last + 1) + last->size;
        if (last->free && last_end == (uint8_t*)block)
        {
            last->size += got;
        }
        else
        {
            last->next = block;
        }
    }

    heap_brk = new_brk;
    return true;
}

void heap_init(void* start, size_t size)
{
    // Legacy entry point kept for compatibility. The heap now grows on
    // demand via brk, so the fixed region is ignored.
    (void)start;
    (void)size;
    heap_start = (BlockHeader*)NULL;
    heap_brk = 0;
}

void* malloc(size_t size)
{
    if (size == 0) return NULL;
    size = align8(size);

    // First pass: look for a fitting free block
    BlockHeader* current = heap_start;
    while (current)
    {
        if (current->free && current->size >= size)
        {
            if (current->size >= size + HEADER_SIZE + MIN_BLOCK_DATA)
            {
                BlockHeader* new_block = (BlockHeader*)((uint8_t*)(current + 1) + size);
                new_block->size = current->size - size - HEADER_SIZE;
                new_block->free = true;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->free = false;
            return (void*)(current + 1);
        }
        current = current->next;
    }

    // No fitting block: grow the heap and retry once
    if (!heap_grow(size + HEADER_SIZE))
        return NULL;

    current = heap_start;
    while (current)
    {
        if (current->free && current->size >= size)
        {
            if (current->size >= size + HEADER_SIZE + MIN_BLOCK_DATA)
            {
                BlockHeader* new_block = (BlockHeader*)((uint8_t*)(current + 1) + size);
                new_block->size = current->size - size - HEADER_SIZE;
                new_block->free = true;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->free = false;
            return (void*)(current + 1);
        }
        current = current->next;
    }

    return NULL;
}

void free(void* ptr)
{
    if (!ptr) return;

    BlockHeader* block = (BlockHeader*)ptr - 1;
    block->free = true;

    // Coalesce adjacent free blocks
    BlockHeader* current = heap_start;
    while (current)
    {
        if (current->free && current->next && current->next->free)
        {
            current->size += HEADER_SIZE + current->next->size;
            current->next = current->next->next;
            continue;
        }
        current = current->next;
    }
}
