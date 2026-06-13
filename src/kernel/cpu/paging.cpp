#include "../../include/cpu/paging.h"
#include "../../include/mm/memory.h"

namespace paging {
    uint64_t *PageTableAddr = 0x00;
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;

    void init(uint64_t *TableAddr)
    {
        memory::memset((uint8_t*)TableAddr, 0x00, 1 + 32 + 16384);

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
        PageTableAddr = PD;
        for (uint64_t i = 0; i < 16384; i++)
        {
            *(PD + i) = ((uint64_t)(0x0000 + i * 0x200000)) | flags | PAGE_SIZE;
        }

        asm volatile("mov %0, %%cr3" :: "r"(PML4) : "memory");
    }

    void allocate_pages(uint64_t virt, uint64_t phys, uint64_t size)
    {
        uint64_t offset = virt / PAGE_SIZE_BYTES;
        for (uint64_t i = 0; i < size; i++)
        {
            *(PageTableAddr + offset + i) = ((uint64_t)(phys + i * PAGE_SIZE_BYTES)) | flags | PAGE_SIZE;
        }

        asm volatile("mov %%cr3, %%rax" ::: "rax", "memory");
        asm volatile("mov %%rax, %%cr3" ::: "memory");
    }

    uint64_t get_phys_addr(uint64_t virt)
    {
        // Identity mapping
        return virt;
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