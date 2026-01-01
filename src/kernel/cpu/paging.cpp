#include "../../include/cpu/paging.h"

namespace paging {
    UINT64 *PageTableAddr = 0x00;
    UINT64 flags = PAGE_PRESENT | PAGE_WRITE;

    void init(UINT64 *TableAddr)
    {
        memory::memset((UINT8*)TableAddr, 0x00, 1 + 32 + 16384);

        // PML4 table
        UINT64 *PML4 = (UINT64*)TableAddr;
        *PML4 = ((UINT64)PML4 + 0x1000) | flags;

        // PDP table
        UINT64 *PDP = (UINT64*)((UINT64)PML4 + 0x1000);
        for (UINT64 i = 0; i < 32; i++)
        {
            *(PDP + i) = (((UINT64)PDP + 0x1000) + i * 0x1000) | flags;
        }

        // PD tables
        UINT64 *PD = (UINT64 *)((UINT64)PDP + 0x1000);
        PageTableAddr = PD;
        for (UINT64 i = 0; i < 16384; i++)
        {
            *(PD + i) = ((UINT64)(0x0000 + i * 0x200000)) | flags | PAGE_SIZE;
        }

        asm volatile("mov %0, %%cr3" :: "r"(PML4) : "memory");
    }

    void allocate_pages(UINT64 virt, UINT64 phys, UINT64 size)
    {
        UINT64 offset = virt / PAGE_SIZE_BYTES;
        for (UINT64 i = 0; i < size; i++)
        {
            *(PageTableAddr + offset + i) = ((UINT64)(phys + i * PAGE_SIZE_BYTES)) | flags | PAGE_SIZE;
        }

        asm volatile("mov %%cr3, %%rax" ::: "rax", "memory");
        asm volatile("mov %%rax, %%cr3" ::: "memory");
    }

    UINT64 get_phys_addr(UINT64 virt)
    {
        // Now: virt = phys
        return virt;
    }

} // namespace