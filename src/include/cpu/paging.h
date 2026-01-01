#ifndef PAGING_H
#define PAGING_H

#include "types.h"
#include "../mm/memory.h"

#define PAGE_SIZE_BYTES 0x200000

#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_ACCESSED   (1 << 5)
#define PAGE_DIRTY      (1 << 6)
#define PAGE_SIZE       (1 << 7)

namespace paging {
    void init(UINT64*);
    void allocate_pages(UINT64 virt, UINT64 phys, UINT64 size);
    UINT64 get_phys_addr(UINT64 virt);
}

#endif  // PAGING_H