#include "../../include/mm/heap.h"
#include "../../include/mm/pmm.h"
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

// Append a new free chunk to the block list. If it is physically adjacent
// to the last block, merge into it instead.
static void heap_append_chunk(uint8_t* chunk, size_t size)
{
    BlockHeader* last = heap_start;
    while (last->next)
        last = last->next;

    // Physically adjacent to a free last block? Merge into it.
    uint8_t* last_end = (uint8_t*)(last + 1) + last->size;
    if (last->free && last_end == chunk)
    {
        last->size += size;
        return;
    }

    // Not adjacent: insert a separate block after the last one
    BlockHeader* block = (BlockHeader*)chunk;
    block->size = size - HEADER_SIZE;
    block->free = true;
    block->next = nullptr;
    last->next = block;
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

        // Kernel runs identity-mapped: physical address is usable directly.
        heap_append_chunk((uint8_t*)phys, GROW_FRAMES * FRAME_SIZE);
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

// Search for a free block of sufficient size, split it if it's too large,
// and return a pointer to the data area.
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

    uart::printf("kmalloc: out of memory, requested %u\n", size);
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