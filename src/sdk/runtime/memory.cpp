// Memory: pages from the kernel and the program's heap.
//
// The heap is a list of free blocks in address order. Every block, free or
// in use, starts with a 16-byte header: its size (header included) and a
// tag saying which it is. Allocate takes the first free block that is big
// enough and splits off what it does not need; Free puts the block back
// and merges it with free neighbours. When no block fits, the heap grows
// by at least HEAP_GROW_PAGES pages. Pages are never given back.
//
// Threads share the heap: every change to it happens under HeapLock.

#include "runtime.h"

namespace
{
    const uint64_t HEADER          = 16;
    const uint64_t MIN_BLOCK       = 32;    // header + room for the list link
    const uint64_t HEAP_GROW_PAGES = 16;    // 64 KiB
    const uint64_t MAX_ALLOCATION  = 1ULL << 40;

    const uint64_t TAG_USED = 0x444553554B4C4253ULL;   // "SBLKUSED"
    const uint64_t TAG_FREE = 0x454552464B4C4253ULL;   // "SBLKFREE"

    struct Block
    {
        uint64_t Size;
        uint64_t Tag;
        Block*   Next;      // free blocks only: the next free block
    };

    Block* FreeList;        // in address order

    // A plain spin lock until SfSync has a mutex: on one CPU a waiter spins
    // out its time slice, then the holder runs on and lets go.
    volatile uint32_t HeapLock;

    void Lock()
    {
        while (__atomic_exchange_n(&HeapLock, 1, __ATOMIC_ACQUIRE))
            asm volatile("pause");
    }

    void Unlock()
    {
        __atomic_store_n(&HeapLock, 0, __ATOMIC_RELEASE);
    }

    char* End(Block* B)
    {
        return (char*)B + B->Size;
    }

    // Put B on the free list and merge it with the blocks right before and
    // after it.
    void Release(Block* B)
    {
        B->Tag = TAG_FREE;

        Block* Prev = nullptr;
        Block* Next = FreeList;
        while (Next && Next < B)
        {
            Prev = Next;
            Next = Next->Next;
        }

        B->Next = Next;
        if (Prev)
            Prev->Next = B;
        else
            FreeList = B;

        if (Next && End(B) == (char*)Next)
        {
            B->Size += Next->Size;
            B->Next = Next->Next;
        }
        if (Prev && End(Prev) == (char*)B)
        {
            Prev->Size += B->Size;
            Prev->Next = B->Next;
        }
    }

    // Add at least Need bytes of fresh pages to the heap.
    SfStatus Grow(uint64_t Need)
    {
        uint64_t Pages = (Need + SF_PAGE_SIZE - 1) / SF_PAGE_SIZE;
        if (Pages < HEAP_GROW_PAGES)
            Pages = HEAP_GROW_PAGES;

        void* Address = nullptr;
        SfStatus Status = MemoryAllocatePages(nullptr, Pages, &Address);
        if (SF_ERROR(Status))
            return Status;

        Block* B = (Block*)Address;
        B->Size = Pages * SF_PAGE_SIZE;
        Release(B);
        return SF_SUCCESS;
    }

    // Take Need bytes from the first free block that has them, or nullptr.
    Block* Take(uint64_t Need)
    {
        Block* Prev = nullptr;
        for (Block* B = FreeList; B; Prev = B, B = B->Next)
        {
            if (B->Size < Need)
                continue;

            Block* Rest = B->Next;
            if (B->Size - Need >= MIN_BLOCK)
            {
                // Split: the tail stays free, in B's place on the list.
                Block* Tail = (Block*)((char*)B + Need);
                Tail->Size = B->Size - Need;
                Tail->Tag  = TAG_FREE;
                Tail->Next = B->Next;
                Rest = Tail;
                B->Size = Need;
            }
            if (Prev)
                Prev->Next = Rest;
            else
                FreeList = Rest;

            B->Tag = TAG_USED;
            return B;
        }
        return nullptr;
    }
}

SfStatus MemoryAllocatePages(SfMemory*, uint64_t Count, void** Address)
{
    return SfCall(SFCALL_MEMORY_ALLOCATE_PAGES, Count, (uint64_t)Address);
}

SfStatus MemoryFreePages(SfMemory*, void* Address, uint64_t Count)
{
    return SfCall(SFCALL_MEMORY_FREE_PAGES, (uint64_t)Address, Count);
}

SfStatus MemoryAllocate(SfMemory*, uint64_t Size, void** Buffer)
{
    if (!Buffer)
        return SF_INVALID_PARAMETER;
    if (Size > MAX_ALLOCATION)
        return SF_OUT_OF_RESOURCES;

    uint64_t Need = HEADER + ((Size + 15) & ~15ULL);
    if (Need < MIN_BLOCK)
        Need = MIN_BLOCK;

    Lock();
    Block* B = Take(Need);
    if (!B)
    {
        SfStatus Status = Grow(Need);
        if (SF_ERROR(Status))
        {
            Unlock();
            return Status;
        }
        B = Take(Need);
    }
    Unlock();

    void* Data = (char*)B + HEADER;
    memset(Data, 0, B->Size - HEADER);
    *Buffer = Data;
    return SF_SUCCESS;
}

SfStatus MemoryFree(SfMemory*, void* Buffer)
{
    if (!Buffer || ((uint64_t)Buffer & 15))
        return SF_INVALID_PARAMETER;
    Block* B = (Block*)((char*)Buffer - HEADER);
    Lock();
    if (B->Tag != TAG_USED)
    {
        Unlock();
        return SF_INVALID_PARAMETER;    // not from Allocate, or freed already
    }
    Release(B);
    Unlock();
    return SF_SUCCESS;
}

extern "C" void* memset(void* Dest, int Value, uint64_t Size)
{
    uint8_t* D = (uint8_t*)Dest;
    for (uint64_t i = 0; i < Size; i++)
        D[i] = (uint8_t)Value;
    return Dest;
}

extern "C" void* memcpy(void* Dest, const void* Src, uint64_t Size)
{
    uint8_t*       D = (uint8_t*)Dest;
    const uint8_t* S = (const uint8_t*)Src;
    for (uint64_t i = 0; i < Size; i++)
        D[i] = S[i];
    return Dest;
}
