#ifndef SFOS_MEMORY_H
#define SFOS_MEMORY_H

#include "table.h"

/// The program's memory: whole pages, and a heap of small blocks.
typedef struct SfMemory SfMemory;

struct SfMemory
{
    SfTableHeader Hdr;

    /// Maps Count zeroed 4 KiB pages; *Address gets the first.
    ///
    /// SF_OUT_OF_RESOURCES when there is not that much memory.
    SfStatus (*AllocatePages)(SfMemory* This, uint64_t Count, void** Address);

    /// Gives back Count pages from Address (as AllocatePages returned
    /// them).
    SfStatus (*FreePages)(SfMemory* This, void* Address, uint64_t Count);

    /// Takes Size bytes from the program's heap, 16-byte aligned and
    /// zeroed; *Buffer gets them.
    SfStatus (*Allocate)(SfMemory* This, uint64_t Size, void** Buffer);

    /// Gives a block from Allocate back.
    ///
    /// SF_INVALID_PARAMETER for anything else.
    SfStatus (*Free)(SfMemory* This, void* Buffer);

    /// Gives Buffer (from Allocate, or null for a new block) Size bytes;
    /// *NewBuffer gets the block, which may have moved.
    ///
    /// What it held stays, up to Size; bytes added are zeroed. On failure
    /// Buffer is left as it was. SF_INVALID_PARAMETER for a Buffer not from
    /// Allocate.
    SfStatus (*Reallocate)(SfMemory* This, void* Buffer, uint64_t Size, void** NewBuffer);
};

/// The size of a page (AllocatePages).
#define SF_PAGE_SIZE        4096ULL

#define SF_MEMORY_SIGNATURE SF_SIGNATURE('S', 'F', 'M', 'E', 'M', 'O', 'R', 'Y')

SF_STATIC_ASSERT(SF_OFFSET_OF(SfMemory, AllocatePages) == 16, "SfMemory layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfMemory, Free) == 40, "SfMemory layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfMemory, Reallocate) == 48, "SfMemory layout");
SF_STATIC_ASSERT(sizeof(SfMemory) == 56, "SfMemory layout");

#endif // SFOS_MEMORY_H
