#include "../../include/mm/heap.h"

struct BlockHeader
{
    size_t         size;
    bool           free;
    BlockHeader*   next;
};

static BlockHeader* heap_start = nullptr;

static const size_t HEADER_SIZE = sizeof(BlockHeader);
static const size_t MIN_BLOCK_DATA = 16;

static size_t align8(size_t size)
{
    return (size + 7) & ~(size_t)7;
}

namespace heap
{
    void init(void* start, size_t size)
    {
        heap_start = (BlockHeader*)start;
        heap_start->size = size - HEADER_SIZE;
        heap_start->free = true;
        heap_start->next = nullptr;
    }
} // namespace heap

// Search for a free block of sufficient size, split it if it's too large,
// and return a pointer to the data area.
void* kmalloc(size_t size)
{
    if (size == 0) return nullptr;
    size = align8(size);

    BlockHeader* current = heap_start;

    while (current)
    {
        if (current->free && current->size >= size)
        {
            // Split the block if it's significantly larger than the requested size
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

    return nullptr;
}

// Free a previously allocated block and coalesce adjacent free blocks.
void kfree(void* ptr)
{
    if (!ptr)
        return;

    BlockHeader* block = (BlockHeader*)ptr - 1;
    block->free = true;

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