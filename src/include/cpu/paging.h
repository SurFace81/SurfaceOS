#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGE_SIZE_BYTES 0x200000

#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_ACCESSED   (1 << 5)
#define PAGE_DIRTY      (1 << 6)
#define PAGE_SIZE       (1 << 7)

namespace paging
{
    void        init(uint64_t*);
    void        allocate_pages(uint64_t virt, uint64_t phys, uint64_t size);
    uint64_t    get_phys_addr(uint64_t virt);
    uint64_t    map_mmio_region(uint64_t phys, uint64_t size_bytes);
} // namespace paging

#endif // PAGING_H