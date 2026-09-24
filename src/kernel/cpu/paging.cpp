// 4-level x86-64 paging.
//
// See paging.h for the address space layout. Three rules hold everywhere in
// this file:
//
//  * Page tables are reached through the direct map: an entry holds a
//    physical address, table() turns it into a pointer. That is what makes
//    the walk below work without a recursive mapping, no matter which
//    address space CR3 currently holds.
//  * 2 MiB pages exist only in the kernel half (direct map, device window,
//    kernel image). User space is 4 KiB pages throughout.
//  * PAGE_NX is only ever set when the CPU reported NX support; bit 63 is a
//    reserved bit (and faults) when EFER.NXE is clear.

#include "../../include/cpu/paging.h"
#include "../../include/cpu/features.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/pmm.h"
#include "../../include/drivers/uart.h"

// Provided by linker.ld: physical end of the kernel image + .bss.
extern "C" uint8_t __kernel_phys_end[];

// init() zeroes and fills the final tables while kentry's boot tables are
// still the live CR3; the two blocks must not overlap.
static_assert(PT_TOTAL_SIZE <= PT_BOOT_OFFSET, "final page tables overlap the boot tables");

namespace paging {

    static uint64_t kernel_pml4_phys = 0;
    static uint8_t* static_tables    = nullptr; // static table block (virtual)
    static uint64_t direct_map_max   = 0;   // exclusive, 2 MiB aligned
    static uint64_t nx_bit           = 0;   // PAGE_NX, or 0 when unsupported

    // Bump allocator for the kernel device window.
    static uint64_t next_device_virt = DEVICE_WINDOW_BASE;

    // Every entry of every kernel-half table: supervisor, global. Global keeps
    // these TLB entries alive across CR3 switches (with CR4.PGE, see
    // cpu::init_features); without PGE the bit is simply ignored.
    static const uint64_t KERNEL_TABLE = PAGE_PRESENT | PAGE_WRITE;
    static const uint64_t KERNEL_LARGE = PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE | PAGE_GLOBAL;

    // The next-level table an entry points to, through the direct map.
    static inline uint64_t* table(uint64_t entry)
    {
        return (uint64_t*)phys_to_virt(entry & PAGE_ADDR_MASK);
    }

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

    // Highest address the kernel has to reach through the direct map.
    // Everything that is real RAM (free, kernel, firmware, ACPI, reserved)
    // counts; memory-mapped IO does not - devices get the device window.
    static uint64_t compute_direct_map_limit(BOOT_HEADER* bh)
    {
        const uint32_t TYPE_MAPPED_IO = 5;

        uint64_t limit = 0;
        uint8_t* map = (uint8_t*)phys_to_virt((uint64_t)bh->MemoryMapAddress);

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
        const uint64_t ceiling = DIRECT_MAP_MAX;

        if (limit < floor)   limit = floor;
        if (limit > ceiling) limit = ceiling;
        return limit;
    }

    // The boot header and its memory map are read through the direct map
    // that kentry.asm's boot tables provide for the first 1 GiB.
    void init(uint64_t tables_phys, BOOT_HEADER* boot_header)
    {
        static_tables = (uint8_t*)phys_to_virt(tables_phys);
        nx_bit = cpu::has_nx() ? PAGE_NX : 0;

        memory::memset(static_tables, 0x00, PT_TOTAL_SIZE);

        uint64_t* pml4      = (uint64_t*)(static_tables + PT_PML4_OFFSET);
        uint64_t* dm_pdpt   = (uint64_t*)(static_tables + PT_DM_PDPT_OFFSET);
        uint64_t* dm_pd     = (uint64_t*)(static_tables + PT_DM_PD_OFFSET);
        uint64_t* dev_pdpt  = (uint64_t*)(static_tables + PT_DEV_PDPT_OFFSET);
        uint64_t* kern_pdpt = (uint64_t*)(static_tables + PT_KERN_PDPT_OFFSET);
        uint64_t* kern_pd   = (uint64_t*)(static_tables + PT_KERN_PD_OFFSET);

        direct_map_max = compute_direct_map_limit(boot_header);

        // --- PML4[273]: direct map of physical RAM, RW, never executable ---
        pml4[DIRECT_MAP_PML4_INDEX] = (tables_phys + PT_DM_PDPT_OFFSET) | KERNEL_TABLE;

        uint64_t dm_pages = direct_map_max / PAGE_SIZE_2M;
        uint64_t dm_pds   = (dm_pages + 511) / 512;

        for (uint64_t i = 0; i < dm_pds; i++)
            dm_pdpt[i] = (tables_phys + PT_DM_PD_OFFSET + i * 0x1000) | KERNEL_TABLE;
        for (uint64_t i = 0; i < dm_pages; i++)
            dm_pd[i] = (i * PAGE_SIZE_2M) | KERNEL_LARGE | nx_bit;

        // --- PML4[256]: device window, empty until map_device() ---
        pml4[DEVICE_PML4_INDEX] = (tables_phys + PT_DEV_PDPT_OFFSET) | KERNEL_TABLE;
        for (uint64_t i = 0; i < PT_DEV_PD_COUNT; i++)
            dev_pdpt[i] = (tables_phys + PT_DEV_PD_OFFSET + i * 0x1000) | KERNEL_TABLE;

        // --- PML4[511]: the kernel image, KERNEL_VMA + phys ---
        // The only executable kernel memory. 2 MiB granularity, so .text,
        // .rodata and .data share a page (as they did under the identity map).
        pml4[KERNEL_PML4_INDEX] = (tables_phys + PT_KERN_PDPT_OFFSET) | KERNEL_TABLE;
        kern_pdpt[KERNEL_PDPT_INDEX] = (tables_phys + PT_KERN_PD_OFFSET) | KERNEL_TABLE;

        uint64_t kernel_first = KERNEL_PHYS_BASE / PAGE_SIZE_2M;
        uint64_t kernel_last  = ((uint64_t)__kernel_phys_end + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M;
        for (uint64_t i = kernel_first; i < kernel_last; i++)
            kern_pd[i] = (i * PAGE_SIZE_2M) | KERNEL_LARGE;

        // PML4[0..255] stay empty: from here on a physical address used as
        // a pointer faults instead of quietly working.
        kernel_pml4_phys = tables_phys + PT_PML4_OFFSET;
        asm volatile("mov %0, %%cr3" :: "r"(kernel_pml4_phys) : "memory");
    }

    uint64_t direct_map_limit()
    {
        return direct_map_max;
    }

    uint64_t map_device(uint64_t phys, uint64_t size, uint64_t cache)
    {
        if (!size)
            return 0;

        uint64_t phys_aligned = phys & ~(PAGE_SIZE_2M - 1);
        uint64_t offset       = phys - phys_aligned;
        uint64_t pages        = (size + offset + PAGE_SIZE_2M - 1) / PAGE_SIZE_2M;

        uint64_t virt = next_device_virt;
        if (virt + pages * PAGE_SIZE_2M > DEVICE_WINDOW_BASE + DEVICE_WINDOW_SIZE)
        {
            uart::printf("paging: kernel device window exhausted\n");
            return 0;
        }
        next_device_virt += pages * PAGE_SIZE_2M;

        uint64_t* dev_pd = (uint64_t*)(static_tables + PT_DEV_PD_OFFSET);

        for (uint64_t i = 0; i < pages; i++)
        {
            uint64_t v = virt + i * PAGE_SIZE_2M;
            uint64_t slot = (v - DEVICE_WINDOW_BASE) / PAGE_SIZE_2M;

            dev_pd[slot] = (phys_aligned + i * PAGE_SIZE_2M)
                         | KERNEL_LARGE | cache | nx_bit;
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
        uint64_t *pml4 = table(read_cr3());
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;

        uint64_t *pdpt = table(entry);
        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        if (entry & PAGE_SIZE)    // 1 GiB huge page
            return (entry & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFF);

        uint64_t *pd = table(entry);
        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        if (entry & PAGE_SIZE)    // 2 MiB huge page
            return (entry & PAGE_ADDR_MASK) | (virt & 0x1FFFFF);

        uint64_t *pt = table(entry);
        entry = pt[pt_index(virt)];
        if (!(entry & PAGE_PRESENT))
            return 0;
        return (entry & PAGE_ADDR_MASK) | (virt & 0xFFF);
    }

    // Walk to the PTE for `virt`, or nullptr when the path is absent or ends
    // in a huge page.
    static uint64_t* find_pte(uint64_t virt)
    {
        uint64_t *pml4 = table(read_cr3());
        uint64_t entry = pml4[pml4_index(virt)];
        if (!(entry & PAGE_PRESENT)) return nullptr;

        uint64_t *pdpt = table(entry);
        entry = pdpt[pdpt_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return nullptr;

        uint64_t *pd = table(entry);
        entry = pd[pd_index(virt)];
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_SIZE)) return nullptr;

        uint64_t *pt = table(entry);
        return &pt[pt_index(virt)];
    }

    bool is_user_range(uint64_t addr, uint64_t len)
    {
        if (addr < USER_MIN || addr >= USER_LIMIT)
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

    // Allocate a zeroed page-table frame; 0 on failure.
    static uint64_t alloc_table()
    {
        uint64_t frame = pmm::alloc_frame();
        if (frame)
            memory::memset((uint8_t*)phys_to_virt(frame), 0x00, 4096);
        return frame;
    }

    // Allocate (or reuse) the next-level table for a 4 KiB mapping walk.
    static uint64_t* next_table(uint64_t* parent, uint64_t index, uint64_t flags)
    {
        if (parent[index] & PAGE_PRESENT)
            return table(parent[index]);

        uint64_t frame = alloc_table();
        if (!frame)
            return nullptr;

        parent[index] = frame | flags;
        return table(frame);
    }

    // NOTE: maps into the currently active address space (cr3).
    // Switch to the target process address space before mapping its pages.
    bool map_page(uint64_t virt, uint64_t phys, uint64_t flags)
    {
        uint64_t *pml4 = table(read_cr3());

        // A new upper-half PML4 entry would only exist in this address
        // space; the kernel half is fixed by init() (see paging.h).
        if (pml4_index(virt) >= USER_PML4_COUNT && !(pml4[pml4_index(virt)] & PAGE_PRESENT))
        {
            uart::printf("paging: map_page(%llx): kernel PML4 slot not set up\n", virt);
            return false;
        }

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
        // Without this check a crafted ELF could map page 0 (turning a NULL
        // dereference into a read of its own data) or walk an upper-half
        // PML4 entry - which every address space shares with the kernel -
        // and punch user-accessible entries into the kernel's own tables.
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

    // Deep-copy one level of the user half. `level` is that of `src`
    // (3 = PDPT, 2 = PD, 1 = PT); PT entries are data pages and get a fresh
    // frame with the same contents. Entries keep their flags.
    static bool clone_level(uint64_t* dst, const uint64_t* src, int level)
    {
        for (uint64_t i = 0; i < 512; i++)
        {
            uint64_t e = src[i];
            if (!(e & PAGE_PRESENT))
                continue;

            if (level == 1)
            {
                uint64_t frame = pmm::alloc_frame();
                if (!frame)
                    return false;
                memory::memcpy((uint8_t*)phys_to_virt(frame),
                               (const uint8_t*)table(e), 4096);
                dst[i] = frame | (e & ~PAGE_ADDR_MASK);
                continue;
            }

            if (e & PAGE_SIZE)
                continue;       // user space has no huge pages

            uint64_t frame = alloc_table();
            if (!frame)
                return false;
            dst[i] = frame | (e & ~PAGE_ADDR_MASK);

            if (!clone_level(table(frame), table(e), level - 1))
                return false;
        }
        return true;
    }

    bool clone_user_space(uint64_t dst_pml4, uint64_t src_pml4)
    {
        // Tables are walked and written through the direct map, so this
        // works no matter which address space CR3 currently holds.
        uint64_t* src = table(src_pml4);
        uint64_t* dst = table(dst_pml4);

        for (uint64_t i = 0; i < USER_PML4_COUNT; i++)
        {
            if (!(src[i] & PAGE_PRESENT))
                continue;

            uint64_t frame = alloc_table();
            if (!frame)
                return false;
            dst[i] = frame | (src[i] & ~PAGE_ADDR_MASK);

            if (!clone_level(table(frame), table(src[i]), 3))
                return false;
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
        uint64_t pml4 = alloc_table();
        if (!pml4)
            return 0;

        // Share the kernel by copying the upper half of its PML4: the tables
        // below those entries are the kernel's own, so later changes inside
        // them (map_device) are visible everywhere. None carries PAGE_USER.
        // The lower half starts empty; map_page builds it on demand.
        uint64_t *kpml4 = table(kernel_pml4_phys);
        uint64_t *upml4 = table(pml4);
        for (uint64_t i = USER_PML4_COUNT; i < 512; i++)
            upml4[i] = kpml4[i];

        return pml4;
    }

    // Free a user page-table subtree: every table and every data page below
    // `table_phys` (a table of `level`: 3 = PDPT, 2 = PD, 1 = PT), then the
    // table itself.
    static void destroy_level(uint64_t table_phys, int level)
    {
        uint64_t *t = table(table_phys);

        for (uint64_t i = 0; i < 512; i++)
        {
            uint64_t e = t[i];
            if (!(e & PAGE_PRESENT))
                continue;

            if (level == 1)
                pmm::free_frame(e & PAGE_ADDR_MASK);
            else if (level == 2 && (e & PAGE_SIZE))
                pmm::free_frames(e & PAGE_ADDR_MASK, 512);   // 2 MiB page
            else if (!(e & PAGE_SIZE))
                destroy_level(e & PAGE_ADDR_MASK, level - 1);
        }
        pmm::free_frame(table_phys);
    }

    void destroy_address_space(uint64_t pml4_phys)
    {
        if (!pml4_phys || pml4_phys == kernel_pml4_phys)
            return;

        // Lower half only: the upper half belongs to the kernel.
        uint64_t *pml4 = table(pml4_phys);
        for (uint64_t i = 0; i < USER_PML4_COUNT; i++)
            if (pml4[i] & PAGE_PRESENT)
                destroy_level(pml4[i] & PAGE_ADDR_MASK, 3);

        pmm::free_frame(pml4_phys);
    }

    void switch_address_space(uint64_t pml4_phys)
    {
        if (!pml4_phys || read_cr3() == pml4_phys)
            return;

        asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    }

} // namespace
