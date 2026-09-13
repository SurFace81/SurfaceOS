#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGE_SIZE_4K    0x1000ULL
#define PAGE_SIZE_2M    0x200000ULL

// Legacy alias: kernel identity mapping operates in 2 MiB huge pages
#define PAGE_SIZE_BYTES PAGE_SIZE_2M

#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_ACCESSED   (1 << 5)
#define PAGE_DIRTY      (1 << 6)
#define PAGE_SIZE       (1 << 7)

#define PAGE_ADDR_MASK  0x000FFFFFFFFFF000ULL

// Kernel identity mapping lives in PML4 entry 0.
// User spaces live in PML4 entry USER_PML4_INDEX, far away from any
// kernel virtual address, so kernel mappings can be shared between
// all address spaces by copying a single PML4 entry.
// USER_BASE must equal the SDK app link address (see src/sdk/linker.ld).
#define USER_PML4_INDEX 8
#define USER_BASE       ((uint64_t)USER_PML4_INDEX << 39)  // 0x40000000000

namespace paging
{
    // Build kernel identity mapping (2 MiB pages, 0..32 GB) at table_addr.
    void        init(uint64_t* table_addr);

    // Legacy 2 MiB mapping: size is a number of 2 MiB pages.
    void        allocate_pages(uint64_t virt, uint64_t phys, uint64_t size);

    // Map MMIO physical range, returns virtual address to use.
    uint64_t    map_mmio_region(uint64_t phys, uint64_t size_bytes);

    // 4 KiB mapping; allocates intermediate tables from PMM.
    // flags: PAGE_PRESENT|PAGE_WRITE|PAGE_USER etc.
    bool        map_page(uint64_t virt, uint64_t phys, uint64_t flags);
    void        unmap_page(uint64_t virt);

    // Full page-table walk (handles 2 MiB huge pages and 4 KiB pages).
    // Returns physical address or 0 if not mapped.
    uint64_t    virtual_to_phys(uint64_t virt);

    // Legacy alias for virtual_to_phys (kernel runs identity-mapped).
    uint64_t    get_phys_addr(uint64_t virt);

    void        invalidate_page(uint64_t virt);

    // Address spaces (one PML4 per process).
    uint64_t    kernel_pml4();
    uint64_t    create_address_space();     // returns PML4 phys addr
    void        destroy_address_space(uint64_t pml4_phys);
    void        switch_address_space(uint64_t pml4_phys);
} // namespace paging

#endif // PAGING_H
