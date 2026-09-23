// 4-level x86-64 paging.
//
// See paging.h for the address space layout. Three rules hold everywhere in
// this file:
//
//  * Page tables always live in identity-mapped RAM, so a physical table
//    address can be dereferenced directly. That is what makes the walk below
//    work without a recursive mapping.
//  * Nothing outside PML4[0] is ever mapped with 2 MiB pages except the
//    kernel device window.
//  * PAGE_NX is only ever set when the CPU reported NX support; bit 63 is a
//    reserved bit (and faults) when EFER.NXE is clear.

#include "../../include/cpu/paging.h"
#include "../../include/cpu/features.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/pmm.h"
#include "../../include/drivers/uart.h"

// Provided by linker.ld; marks the end of the kernel image + .bss.
extern "C" uint8_t __kernel_end[];

namespace paging {

    static uint64_t kernel_pml4_phys = 0;
    static uint64_t static_tables    = 0;   // base of the static table block
    static uint64_t identity_max     = 0;   // exclusive, 2 MiB aligned
    static uint64_t nx_bit           = 0;   // PAGE_NX, or 0 when unsupported

    // Bump allocator for the kernel device window.
    static uint64_t next_device_virt = KERNEL_VIRT_BASE;

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

    // Highest address the kernel has to reach through the identity map.
    // Everything that is real RAM (free, kernel, firmware, ACPI, reserved)
    // counts; memory-mapped IO does not - devices get the kernel window.
    static uint64_t compute_identity_limit(BOOT_HEADER* bh)
    {
        const uint32_t TYPE_MAPPED_IO = 5;

        uint64_t limit = 0;
        uint8_t* map = (uint8_t*)bh->MemoryMapAddress;

        for (uint64_t i = 0; i < bh->MemoryMapEntriesNumber; i++)
        {
            MEMORY_MAP_ENTRY* e = (MEMORY_MAP_ENTRY*)(map + i * bh->MemoryMapEntrySize);
            if (e->Type == TYPE_MAPPED_IO)
                continue;
            if (e->End > limit)
                limit = e->End;
        }

        // Round up to a 2 MiB page, then clamp into what the static PDs can
        // describe. Keep a floor so a bogus memory map cannot leave the
        // kernel image or the static page tables unmapped.
        limit = (limit + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);

        const uint64_t floor = 64ULL * 1024 * 1024;
        const uint64_t ceiling = (uint64_t)PT_ID_PD_COUNT * 512 * PAGE_SIZE_2M;  // 32 GiB

        if (limit < floor)   limit = floor;
        if (limit > ceiling) limit = ceiling;
        return limit;
    }

    void init(uint64_t* table_base, BOOT_HEADER* boot_header)
    {
        uint64_t base = (uint64_t)table_base;

        static_tables = base;
        nx_bit = cpu::has_nx() ? PAGE_NX : 0;

        memory::memset((uint8_t*)base, 0x00, PT_TOTAL_SIZE);

        uint64_t* pml4      = (uint64_t*)(base + PT_PML4_OFFSET);
        uint64_t* id_pdpt   = (uint64_t*)(base + PT_ID_PDPT_OFFSET);
        uint64_t* id_pd     = (uint64_t*)(base + PT_ID_PD_OFFSET);
        uint64_t* kern_pdpt = (uint64_t*)(base + PT_KERN_PDPT_OFFSET);
        uint64_t* kern_pd   = (uint64_t*)(base + PT_KERN_PD_OFFSET);

        identity_max = compute_identity_limit(boot_header);

        // --- PML4[0]: identity map of physical RAM, supervisor only ---
        pml4[0] = (uint64_t)id_pdpt | PAGE_PRESENT | PAGE_WRITE;

        uint64_t id_pages = identity_max / PAGE_SIZE_2M;          // 2 MiB pages
        uint64_t id_pds   = (id_pages + 511) / 512;

        for (uint64_t i = 0; i < id_pds; i++)
            id_pdpt[i] = ((uint64_t)id_pd + i * 0x1000) | PAGE_PRESENT | PAGE_WRITE;

        // Only the pages holding the kernel image stay executable; the heap,
        // the stacks, the page tables and the screen back buffer all get NX.
        uint64_t kernel_first = 0x200000 / PAGE_SIZE_2M;
        uint64_t kernel_last  = ((uint64_t)__kernel_end + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M;

        for (uint64_t i = 0; i < id_pages; i++)
        {
            uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE;
            if (i < kernel_first || i >= kernel_last)
                flags |= nx_bit;
            id_pd[i] = (i * PAGE_SIZE_2M) | flags;
        }

        // --- PML4[256]: kernel device window, empty for now ---
        pml4[KERNEL_PML4_INDEX] = (uint64_t)kern_pdpt | PAGE_PRESENT | PAGE_WRITE;
        for (uint64_t i = 0; i < PT_KERN_PD_COUNT; i++)
            kern_pdpt[i] = ((uint64_t)kern_pd + i * 0x1000) | PAGE_PRESENT | PAGE_WRITE;

        kernel_pml4_phys = (uint64_t)pml4;
        asm volatile("mov %0, %%cr3" :: "r"(pml4) : "memory");
    }

    uint64_t identity_limit()
    {
        return identity_max;
    }

    uint64_t map_device(uint64_t phys, uint64_t size, uint64_t cache)
    {
        if (!size)
            return 0;

        uint64_t phys_aligned = phys & ~(PAGE_SIZE_2M - 1);
        uint64_t offset       = phys - phys_aligned;
        uint64_t pages        = (size + offset + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M;

        uint64_t virt = next_device_virt;
        if (virt + pages * PAGE_SIZE_2M > KERNEL_VIRT_BASE + KERNEL_WINDOW_SIZE)
        {
            uart::printf("paging: kernel device window exhausted\n");
            return 0;
        }
        next_device_virt += pages * PAGE_SIZE_2M;

        uint64_t* kern_pd = (uint64_t*)(static_tables + PT_KERN_PD_OFFSET);

        for (uint64_t i = 0; i < pages; i++)
        {
            uint64_t v = virt + i * PAGE_SIZE_2M;
            uint64_t slot = (v - KERNEL_VIRT_BASE) / PAGE_SIZE_2M;

            kern_pd[slot] = (phys_aligned + i * PAGE_SIZE_2M)
                          | PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE | cache | nx_bit;
            invalidate_page(v);
        }

        return virt + offset;
    }

    uint64_t map_mmio_region(uint64_t phys, uint64_t size)
    {
        return map_device(phys, size, PAGE_CACHE_UC);
    }

    uint64_t map_framebuffer(uint64_t phys, uint64_t size)
    {
        // Write-combining: screen::flush() pushes the whole back buffer with
        // rep movsq ~45 times a second. UC would make that take longer than
        // the frame it is drawing; WB would let the panel scan out stale
        // data because the display engine does not snoop the CPU caches.
        return map_device(phys, size, cpu::has_pat() ? PAGE_CACHE_WC_2M : PAGE_CACHE_UC);
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

    // Walk to the PTE for `virt`, or nullptr when the path is absent or ends
    // in a huge page.
    static uint64_t* find_pte(uint64_t virt)
    {
        uint64_t *pml4 = (uint64_t*)read_cr3();
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT)) return nullptr;

        uint64_t *pdpt = (uint64_t*)(entry & PAGE_ADDR_MASK);
        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return nullptr;

        uint64_t *pd = (uint64_t*)(entry & PAGE_ADDR_MASK);
        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return nullptr;

        uint64_t *pt = (uint64_t*)(entry & PAGE_ADDR_MASK);
        return &pt[pt_index(virt)];
    }

    bool is_user_range(uint64_t addr, uint64_t len)
    {
        if (addr < USER_BASE || addr >= USER_LIMIT)
            return false;
        return len <= USER_LIMIT - addr;    // cannot overflow, addr < USER_LIMIT
    }

    bool user_page_accessible(uint64_t virt, bool write)
    {
        uint64_t* pte = find_pte(virt);
        if (!pte)
            return false;

        uint64_t e = *pte;
        if (!(e & PAGE_PRESENT) || !(e & PAGE_USER))
            return false;
        if (write && !(e & PAGE_WRITE))
            return false;
        return true;
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
        // page is (the CPU ANDs the U/S bit along the walk), and must never
        // be NX (the CPU ORs the NX bit along the walk).
        uint64_t table_flags = PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);

        uint64_t *pdpt = next_table(pml4, pml4_index(virt), table_flags);
        if (!pdpt) return false;
        uint64_t *pd = next_table(pdpt, pdpt_index(virt), table_flags);
        if (!pd) return false;
        uint64_t *pt = next_table(pd, pd_index(virt), table_flags);
        if (!pt) return false;

        if (!cpu::has_nx())
            flags &= ~PAGE_NX;

        pt[pt_index(virt)] = (phys & PAGE_ADDR_MASK) | (flags | PAGE_PRESENT);
        invalidate_page(virt);
        return true;
    }

    bool map_user_page(uint64_t virt, uint64_t phys, uint64_t flags)
    {
        // Without this check a crafted ELF (p_vaddr in the low canonical
        // range) would walk PML4[0] - which every address space shares with
        // the kernel - and permanently punch user-accessible entries into
        // the kernel's own page tables. destroy_address_space() only tears
        // down PML4[USER_PML4_INDEX], so the damage would outlive the app.
        if (!is_user_range(virt, PAGE_SIZE_4K))
            return false;

        return map_page(virt, phys, flags | PAGE_USER);
    }

    void upgrade_page_flags(uint64_t virt, uint64_t flags)
    {
        uint64_t* pte = find_pte(virt);
        if (!pte)
            return;

        // Permission bits are additive when two ELF segments share a page,
        // but NX is the inverse: a page shared by a readable and an
        // executable segment must end up executable.
        *pte |= (flags & ~PAGE_NX);
        if (!(flags & PAGE_NX))
            *pte &= ~PAGE_NX;

        invalidate_page(virt);
    }

    bool set_user_page_flags(uint64_t virt, uint64_t flags)
    {
        if (!is_user_range(virt, PAGE_SIZE_4K))
            return false;

        uint64_t* pte = find_pte(virt);
        if (!pte || !(*pte & PAGE_PRESENT))
            return false;

        const uint64_t perm = PAGE_USER | PAGE_WRITE | PAGE_NX;
        if (!cpu::has_nx())
            flags &= ~PAGE_NX;

        *pte = (*pte & ~perm) | (flags & perm);
        invalidate_page(virt);
        return true;
    }

    uint64_t page_frame(uint64_t virt)
    {
        uint64_t* pte = find_pte(virt);
        if (!pte || !(*pte & PAGE_PRESENT))
            return 0;
        return *pte & PAGE_ADDR_MASK;
    }

    // Allocate a zeroed table frame for the clone; 0 on failure.
    static uint64_t alloc_table()
    {
        uint64_t frame = pmm::alloc_frame();
        if (frame)
            memory::memset((uint8_t*)frame, 0x00, 4096);
        return frame;
    }

    bool clone_user_space(uint64_t dst_pml4, uint64_t src_pml4)
    {
        uint64_t src_e = ((uint64_t*)src_pml4)[USER_PML4_INDEX];
        uint64_t dst_e = ((uint64_t*)dst_pml4)[USER_PML4_INDEX];
        if (!(src_e & PAGE_PRESENT) || !(dst_e & PAGE_PRESENT))
            return false;

        uint64_t* src_pdpt = (uint64_t*)(src_e & PAGE_ADDR_MASK);
        uint64_t* dst_pdpt = (uint64_t*)(dst_e & PAGE_ADDR_MASK);

        // Tables are walked and written through the identity map, so this
        // works no matter which address space CR3 currently holds.
        for (uint64_t i = 0; i < 512; i++)
        {
            if (!(src_pdpt[i] & PAGE_PRESENT) || (src_pdpt[i] & PAGE_SIZE))
                continue;

            uint64_t pd_frame = alloc_table();
            if (!pd_frame)
                return false;
            dst_pdpt[i] = pd_frame | (src_pdpt[i] & ~PAGE_ADDR_MASK);

            uint64_t* src_pd = (uint64_t*)(src_pdpt[i] & PAGE_ADDR_MASK);
            uint64_t* dst_pd = (uint64_t*)pd_frame;

            for (uint64_t j = 0; j < 512; j++)
            {
                if (!(src_pd[j] & PAGE_PRESENT) || (src_pd[j] & PAGE_SIZE))
                    continue;

                uint64_t pt_frame = alloc_table();
                if (!pt_frame)
                    return false;
                dst_pd[j] = pt_frame | (src_pd[j] & ~PAGE_ADDR_MASK);

                uint64_t* src_pt = (uint64_t*)(src_pd[j] & PAGE_ADDR_MASK);
                uint64_t* dst_pt = (uint64_t*)pt_frame;

                for (uint64_t k = 0; k < 512; k++)
                {
                    if (!(src_pt[k] & PAGE_PRESENT))
                        continue;

                    uint64_t frame = pmm::alloc_frame();
                    if (!frame)
                        return false;

                    memory::memcpy((uint8_t*)frame,
                                   (uint8_t*)(src_pt[k] & PAGE_ADDR_MASK), 4096);
                    dst_pt[k] = frame | (src_pt[k] & ~PAGE_ADDR_MASK);
                }
            }
        }
        return true;
    }

    void unmap_page(uint64_t virt)
    {
        uint64_t* pte = find_pte(virt);
        if (!pte)
            return;

        *pte = 0;
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

        // Share every kernel mapping by copying two PML4 entries: the
        // identity map and the device window. Neither carries PAGE_USER, so
        // ring 3 cannot reach them.
        uint64_t *kpml4 = (uint64_t*)kernel_pml4_phys;
        uint64_t *upml4 = (uint64_t*)pml4;
        upml4[0]                 = kpml4[0];
        upml4[KERNEL_PML4_INDEX] = kpml4[KERNEL_PML4_INDEX];

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

} // namespace
