// Kernel heap: first-fit free list over an initial static region plus chunks
// pulled from the PMM on demand.
//
// The block list is kept sorted by address. That is what makes coalescing
// safe: two neighbours in the list may only be merged when they are also
// physically adjacent. The heap is fed from pmm::alloc_frames(), which hands
// out chunks wherever it finds room, so "next in the list" and "next in
// memory" are not the same thing.

#include "../../include/mm/heap.h"
#include "../../include/mm/pmm.h"
#include "../../include/cpu/paging.h"
#include "../../include/drivers/uart.h"

struct BlockHeader
{
    size_t         size;
    bool           free;
    BlockHeader*   next;
};

static BlockHeader* heap_start = nullptr;

static const size_t HEADER_SIZE = sizeof(BlockHeader);
static const size_t MIN_BLOCK_DATA = 16;

// Heap growth: how many PMM frames each new chunk takes (1 MiB)
static const uint64_t GROW_FRAMES = 256;

static size_t align8(size_t size)
{
    return (size + 7) & ~(size_t)7;
}

// End of a block's payload, i.e. the first byte after it in memory.
static inline uint8_t* block_end(BlockHeader* b)
{
    return (uint8_t*)(b + 1) + b->size;
}

// Merge `b` with its successor when the two are contiguous in memory.
static bool try_merge_with_next(BlockHeader* b)
{
    BlockHeader* n = b->next;
    if (!n || !b->free || !n->free)
        return false;

    if (block_end(b) != (uint8_t*)n)
        return false;   // different chunks with a hole in between

    b->size += HEADER_SIZE + n->size;
    b->next = n->next;
    return true;
}

// Insert a new free chunk, keeping the list sorted by address, and coalesce
// with whichever neighbours turn out to be contiguous.
static void heap_insert_chunk(uint8_t* chunk, size_t size)
{
    BlockHeader* block = (BlockHeader*)chunk;
    block->size = size - HEADER_SIZE;
    block->free = true;
    block->next = nullptr;

    if (!heap_start || block < heap_start)
    {
        block->next = heap_start;
        heap_start = block;
        try_merge_with_next(block);
        return;
    }

    BlockHeader* prev = heap_start;
    while (prev->next && prev->next < block)
        prev = prev->next;

    block->next = prev->next;
    prev->next = block;

    try_merge_with_next(block);
    try_merge_with_next(prev);
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

    // Request a new chunk of physical memory from the PMM and add it
    // to the heap. Returns true on success.
    bool grow()
    {
        uint64_t phys = pmm::alloc_frames(GROW_FRAMES);
        if (!phys)
            return false;

        heap_insert_chunk((uint8_t*)phys_to_virt(phys), GROW_FRAMES * FRAME_SIZE);
        return true;
    }

    void get_stats(HeapStats* out)
    {
        out->total_size = 0;
        out->used_size = 0;
        out->free_size = 0;
        out->block_count = 0;
        out->free_block_count = 0;
        out->used_block_count = 0;
        out->largest_free_block = 0;

        BlockHeader* current = heap_start;
        while (current)
        {
            out->block_count++;
            out->total_size += current->size;

            if (current->free)
            {
                out->free_block_count++;
                out->free_size += current->size;
                if (current->size > out->largest_free_block)
                    out->largest_free_block = current->size;
            }
            else
            {
                out->used_block_count++;
                out->used_size += current->size;
            }

            current = current->next;
        }
    }

} // namespace heap

// Find a free block of sufficient size in the existing list and carve it out.
static void* heap_find_fit(size_t size)
{
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

void* kmalloc(size_t size)
{
    if (size == 0) return nullptr;
    size = align8(size);

    void* ptr = heap_find_fit(size);
    if (ptr)
        return ptr;

    // Out of room: grow the heap from the PMM and retry.
    // A single allocation can exceed one chunk, so grow until it fits
    // or the PMM is exhausted.
    while (heap::grow())
    {
        ptr = heap_find_fit(size);
        if (ptr)
            return ptr;
    }

    uart::printf("kmalloc: out of memory, requested %llu\n", (uint64_t)size);
    return nullptr;
}

// Free a previously allocated block and coalesce with its immediate
// neighbours - but only where they are genuinely adjacent in memory.
void kfree(void* ptr)
{
    if (!ptr)
        return;

    BlockHeader* block = (BlockHeader*)ptr - 1;
    block->free = true;

    // Forward merge is local; the backward merge needs the predecessor, which
    // a singly-linked list only gives us by walking.
    try_merge_with_next(block);

    BlockHeader* prev = heap_start;
    while (prev && prev->next != block)
        prev = prev->next;

    if (prev)
        try_merge_with_next(prev);
}
