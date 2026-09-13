// 4-level x86-64 paging.
//
// Kernel identity mapping (legacy, 2 MiB huge pages in PML4[0]) is kept
// for the kernel itself and all drivers. Per-process user address spaces
// live in PML4[USER_PML4_INDEX] and use 4 KiB pages.

#include "../../include/cpu/paging.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/pmm.h"
#include "../../include/drivers/uart.h"

namespace paging {

    static uint64_t kernel_pml4_phys = 0;
    static uint64_t *legacy_pd = 0;   // kernel PD (2 MiB entries), used by allocate_pages

    static inline uint64_t read_cr3()
    {
        uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        return cr3;
    }

    static inline uint64_t pml4_index(uint64_t virt) { return (virt >> 39) & 0x1FF; }
    static inline uint64_t pdpt_index(uint64_t virt) { return (virt >> 30) & 0x1FF; }
    static inline uint64_t pd_index(uint64_t virt)   { return (virt >> 21) & 0x1FF; }
    static inline uint64_t pt_index(uint64_t virt)   { return (virt >> 12) & 0x1FF; }

    void init(uint64_t *TableAddr)
    {
        memory::memset((uint8_t*)TableAddr, 0x00, 1 + 32 + 16384);

        uint64_t flags = PAGE_PRESENT | PAGE_WRITE;

        // PML4 table
        uint64_t *PML4 = (uint64_t*)TableAddr;
        *PML4 = ((uint64_t)PML4 + 0x1000) | flags;

        // PDP table
        uint64_t *PDP = (uint64_t*)((uint64_t)PML4 + 0x1000);
        for (uint64_t i = 0; i < 32; i++)
        {
            *(PDP + i) = (((uint64_t)PDP + 0x1000) + i * 0x1000) | flags;
        }

        // PD tables
        uint64_t *PD = (uint64_t *)((uint64_t)PDP + 0x1000);
        legacy_pd = PD;
        for (uint64_t i = 0; i < 16384; i++)
        {
            *(PD + i) = ((uint64_t)(0x0000 + i * 0x200000)) | flags | PAGE_SIZE;
        }

        kernel_pml4_phys = (uint64_t)PML4;
        asm volatile("mov %0, %%cr3" :: "r"(PML4) : "memory");
    }

    void allocate_pages(uint64_t virt, uint64_t phys, uint64_t size)
    {
        uint64_t offset = virt / PAGE_SIZE_BYTES;
        for (uint64_t i = 0; i < size; i++)
        {
            *(legacy_pd + offset + i) = ((uint64_t)(phys + i * PAGE_SIZE_BYTES)) | PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE;
        }

        asm volatile("mov %%cr3, %%rax" ::: "rax", "memory");
        asm volatile("mov %%rax, %%cr3" ::: "rax", "memory");
    }

    uint64_t virtual_to_phys(uint64_t virt)
    {
        uint64_t *pml4 = (uint64_t*)read_cr3();
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;

        uint64_t *pdpt = (uint64_t*)(entry & PAGE_ADDR_MASK);
        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        if (entry & PAGE_SIZE)    // 1 GiB huge page
            return (entry & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFF);

        uint64_t *pd = (uint64_t*)(entry & PAGE_ADDR_MASK);
        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        if (entry & PAGE_SIZE)    // 2 MiB huge page
            return (entry & PAGE_ADDR_MASK) | (virt & 0x1FFFFF);

        uint64_t *pt = (uint64_t*)(entry & PAGE_ADDR_MASK);
        entry = pt[pt_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        return (entry & PAGE_ADDR_MASK) | (virt & 0xFFF);
    }

    uint64_t get_phys_addr(uint64_t virt)
    {
        // Kernel runs identity-mapped; keep the cheap path for drivers.
        return virt;
    }

    // Allocate (or reuse) the next-level table for a 4 KiB mapping walk.
    static uint64_t* next_table(uint64_t* table, uint64_t index, uint64_t flags)
    {
        if (table[index] & PAGE_PRESENT)
            return (uint64_t*)(table[index] & PAGE_ADDR_MASK);

        uint64_t frame = pmm::alloc_frame();
        if (!frame)
            return nullptr;

        memory::memset((uint8_t*)frame, 0x00, 4096);
        table[index] = frame | flags;
        return (uint64_t*)frame;
    }

    // NOTE: maps into the currently active address space (cr3).
    // Switch to the target process address space before mapping its pages.
    bool map_page(uint64_t virt, uint64_t phys, uint64_t flags)
    {
        uint64_t *pml4 = (uint64_t*)read_cr3();

        // Intermediate tables must always be user-accessible when the leaf
        // page is (CPU ANDs the U/S bit along the walk).
        uint64_t table_flags = PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);

        uint64_t *pdpt = next_table(pml4, pml4_index(virt), table_flags);
        if (!pdpt) return false;
        uint64_t *pd = next_table(pdpt, pdpt_index(virt), table_flags);
        if (!pd) return false;
        uint64_t *pt = next_table(pd, pd_index(virt), table_flags);
        if (!pt) return false;

        pt[pt_index(virt)] = (phys & PAGE_ADDR_MASK) | (flags | PAGE_PRESENT);
        invalidate_page(virt);
        return true;
    }

    void upgrade_page_flags(uint64_t virt, uint64_t flags)
    {
        uint64_t *pml4 = (uint64_t*)read_cr3();
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT)) return;
        uint64_t *pdpt = (uint64_t*)(entry & PAGE_ADDR_MASK);

        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return;
        uint64_t *pd = (uint64_t*)(entry & PAGE_ADDR_MASK);

        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return;
        uint64_t *pt = (uint64_t*)(entry & PAGE_ADDR_MASK);

        uint64_t idx = pt_index(virt);
        pt[idx] |= flags;
        invalidate_page(virt);
    }

    void unmap_page(uint64_t virt)
    {
        uint64_t *pml4 = (uint64_t*)read_cr3();
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT)) return;
        uint64_t *pdpt = (uint64_t*)(entry & PAGE_ADDR_MASK);

        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return;
        uint64_t *pd = (uint64_t*)(entry & PAGE_ADDR_MASK);

        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return;
        uint64_t *pt = (uint64_t*)(entry & PAGE_ADDR_MASK);

        pt[pt_index(virt)] = 0;
        invalidate_page(virt);
    }

    void invalidate_page(uint64_t virt)
    {
        asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    uint64_t kernel_pml4()
    {
        return kernel_pml4_phys;
    }

    uint64_t create_address_space()
    {
        uint64_t pml4 = pmm::alloc_frame();
        if (!pml4)
            return 0;

        memory::memset((uint8_t*)pml4, 0x00, 4096);

        // Share all kernel mappings (PML4 entry 0 with the identity map).
        // Kernel pages have no PAGE_USER flag, so user mode cannot touch them.
        uint64_t *kpml4 = (uint64_t*)kernel_pml4_phys;
        uint64_t *upml4 = (uint64_t*)pml4;
        upml4[0] = kpml4[0];

        // Fresh empty PDPT for the user region.
        uint64_t user_pdpt = pmm::alloc_frame();
        if (!user_pdpt)
        {
            pmm::free_frame(pml4);
            return 0;
        }
        memory::memset((uint8_t*)user_pdpt, 0x00, 4096);
        upml4[USER_PML4_INDEX] = user_pdpt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;

        return pml4;
    }

    // Free the page-table subtree of one PDPT entry (user region only).
    static void destroy_pdpt_entry(uint64_t pdpt_phys, uint64_t pdpt_idx)
    {
        uint64_t *pdpt = (uint64_t*)pdpt_phys;
        uint64_t pdpt_entry = pdpt[pdpt_idx];
        if (!(pdpt_entry & PAGE_PRESENT))
            return;

        uint64_t pd_phys = pdpt_entry & PAGE_ADDR_MASK;
        uint64_t *pd = (uint64_t*)pd_phys;

        for (uint64_t pd_idx = 0; pd_idx < 512; pd_idx++)
        {
            uint64_t pd_entry = pd[pd_idx];
            if (!(pd_entry & PAGE_PRESENT))
                continue;

            if (pd_entry & PAGE_SIZE)
            {
                // 2 MiB huge page in user space: free the whole region
                pmm::free_frames(pd_entry & PAGE_ADDR_MASK, 512);
                continue;
            }

            uint64_t pt_phys = pd_entry & PAGE_ADDR_MASK;
            uint64_t *pt = (uint64_t*)pt_phys;
            for (uint64_t pt_idx = 0; pt_idx < 512; pt_idx++)
            {
                uint64_t pt_entry = pt[pt_idx];
                if (pt_entry & PAGE_PRESENT)
                    pmm::free_frame(pt_entry & PAGE_ADDR_MASK);
            }
            pmm::free_frame(pt_phys);
        }
        pmm::free_frame(pd_phys);
    }

    void destroy_address_space(uint64_t pml4_phys)
    {
        if (!pml4_phys || pml4_phys == kernel_pml4_phys)
            return;

        uint64_t *pml4 = (uint64_t*)pml4_phys;
        uint64_t entry = pml4[USER_PML4_INDEX];
        if (entry & PAGE_PRESENT)
        {
            uint64_t pdpt_phys = entry & PAGE_ADDR_MASK;
            uint64_t *pdpt = (uint64_t*)pdpt_phys;

            for (uint64_t i = 0; i < 512; i++)
                destroy_pdpt_entry(pdpt_phys, i);

            pmm::free_frame(pdpt_phys);
        }

        pmm::free_frame(pml4_phys);
    }

    void switch_address_space(uint64_t pml4_phys)
    {
        if (!pml4_phys || read_cr3() == pml4_phys)
            return;

        asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    }

    static uint64_t next_mmio_virt = 0x10000000ULL; // 256 MB, free region for MMIO

    uint64_t map_mmio_region(uint64_t phys, uint64_t size_bytes) {
        uint64_t phys_aligned = phys & ~(uint64_t)(PAGE_SIZE_BYTES - 1);
        uint64_t offset_in_page = phys - phys_aligned;
        uint64_t pages = (size_bytes + offset_in_page + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;

        // Align next_mmio_virt to page boundary
        uint64_t virt = next_mmio_virt;
        next_mmio_virt += pages * PAGE_SIZE_BYTES;

        allocate_pages(virt, phys_aligned, pages);

        return virt + offset_in_page;
    }

} // namespace
