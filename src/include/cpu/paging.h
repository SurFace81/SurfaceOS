#ifndef PAGING_H
#define PAGING_H

#include "types.h"
#include "../boot/boot.h"

#define PAGE_SIZE_4K    0x1000ULL
#define PAGE_SIZE_2M    0x200000ULL

// Page table entry bits (Intel SDM Vol.3 Table 4-19/4-20)
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_PWT        (1ULL << 3)
#define PAGE_PCD        (1ULL << 4)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_SIZE       (1ULL << 7)     // PS: this entry maps a huge page
#define PAGE_PAT_4K     (1ULL << 7)     // PAT bit in a 4 KiB PTE
#define PAGE_GLOBAL     (1ULL << 8)
#define PAGE_PAT_2M     (1ULL << 12)    // PAT bit in a 2 MiB PDE
#define PAGE_NX         (1ULL << 63)    // needs EFER.NXE, see cpu::has_nx()

#define PAGE_ADDR_MASK  0x000FFFFFFFFFF000ULL

// Memory types, expressed through the PAT layout installed by
// cpu::init_features(). Index = PAT*4 + PCD*2 + PWT.
//   index 3 -> UC  (strong uncacheable)  - MMIO registers
//   index 7 -> WC  (write combining)     - framebuffer
#define PAGE_CACHE_UC       (PAGE_PCD | PAGE_PWT)
#define PAGE_CACHE_WC_4K    (PAGE_PAT_4K | PAGE_PCD | PAGE_PWT)
#define PAGE_CACHE_WC_2M    (PAGE_PAT_2M | PAGE_PCD | PAGE_PWT)

// ---------------------------------------------------------------------------
// Virtual address space layout
// ---------------------------------------------------------------------------
//
// PML4[0]    identity map of physical RAM, 2 MiB pages, kernel only.
//            Sized to the memory map at boot, never covers MMIO apertures.
// PML4[8]    per-process user space (4 KiB pages, PAGE_USER).
// PML4[256]  kernel device window: framebuffer and MMIO BARs, with explicit
//            cache attributes. Shared by every address space.
//
// Devices used to be mapped by overwriting identity-map entries (framebuffer
// at virt 0x8000000, MMIO at virt 0x10000000). That destroyed the identity
// mapping of the RAM living at those physical addresses while the PMM happily
// kept handing those frames out. It was invisible under `qemu -m 128M`, where
// RAM stops exactly at 0x8000000, and corrupted memory on anything larger.

#define USER_PML4_INDEX   8
#define USER_BASE         ((uint64_t)USER_PML4_INDEX << 39)   // 0x40000000000
#define USER_SPACE_SIZE   0x40000000ULL                       // 1 GiB (one PDPT entry)
#define USER_LIMIT        (USER_BASE + USER_SPACE_SIZE)

#define KERNEL_PML4_INDEX 256
#define KERNEL_VIRT_BASE  0xFFFF800000000000ULL
#define KERNEL_WINDOW_SIZE (4ULL * 1024 * 1024 * 1024)        // 4 GiB, 4 static PDs

// Static page tables carved out of the 5 MB the bootloader reserves at
// 0x300000. Keeping them static means device mappings need no allocator and
// can be set up before the PMM exists.
#define PT_PML4_OFFSET        0x0000      // PML4
#define PT_ID_PDPT_OFFSET     0x1000      // PDPT for PML4[0]
#define PT_ID_PD_OFFSET       0x2000      // 32 PDs -> 32 GiB of identity map
#define PT_ID_PD_COUNT        32
#define PT_KERN_PDPT_OFFSET   0x22000     // PDPT for PML4[256]
#define PT_KERN_PD_OFFSET     0x23000     // 4 PDs -> 4 GiB of device window
#define PT_KERN_PD_COUNT      4
#define PT_TOTAL_SIZE         0x27000

namespace paging
{
    // Build the identity map (2 MiB pages, sized to the memory map) and the
    // empty kernel device window, then load CR3.
    // Run cpu::init_features() first: NX is only set when EFER.NXE is on.
    void        init(uint64_t* table_base, BOOT_HEADER* boot_header);

    // Map a physical device range into the kernel window and return the
    // virtual address to use. `cache` is PAGE_CACHE_UC or PAGE_CACHE_WC_2M.
    uint64_t    map_device(uint64_t phys, uint64_t size, uint64_t cache);

    // Convenience wrappers: MMIO registers are UC, the framebuffer is WC so
    // that the rep-movsq flush in screen::flush() stays fast.
    uint64_t    map_mmio_region(uint64_t phys, uint64_t size);
    uint64_t    map_framebuffer(uint64_t phys, uint64_t size);

    // 4 KiB mapping in the active address space; intermediate tables come
    // from the PMM. map_user_page additionally rejects anything outside
    // [USER_BASE, USER_LIMIT) and forces PAGE_USER, so a malformed ELF can
    // never reach into the kernel-shared PML4[0] subtree.
    bool        map_page(uint64_t virt, uint64_t phys, uint64_t flags);
    bool        map_user_page(uint64_t virt, uint64_t phys, uint64_t flags);
    void        unmap_page(uint64_t virt);

    // Replace the permission bits (PAGE_USER, PAGE_WRITE, PAGE_NX) of a
    // mapped 4 KiB user page, keeping its frame. Clearing PAGE_USER while the
    // page stays present is how PROT_NONE is expressed. False if unmapped.
    bool        set_user_page_flags(uint64_t virt, uint64_t flags);

    // Physical frame backing a mapped 4 KiB page, whether or not it is
    // currently user-accessible. 0 if unmapped.
    uint64_t    page_frame(uint64_t virt);

    // Deep-copy the user half of `src_pml4` into `dst_pml4` (a fresh address
    // space from create_address_space): every present page gets its own new
    // frame with the same contents and permissions. Used by fork(). On
    // failure the partial copy is left for destroy_address_space().
    bool        clone_user_space(uint64_t dst_pml4, uint64_t src_pml4);

    // OR additional permission bits into an already-mapped 4 KiB page, and
    // clear PAGE_NX if the new flags make it executable.
    void        upgrade_page_flags(uint64_t virt, uint64_t flags);

    // Full page-table walk (handles 1 GiB / 2 MiB / 4 KiB). 0 if not mapped.
    uint64_t    virtual_to_phys(uint64_t virt);

    // True if [addr, addr+len) lies entirely inside user space. Overflow-safe.
    bool        is_user_range(uint64_t addr, uint64_t len);

    // True if the 4 KiB page containing `virt` is present, user-accessible,
    // and (when `write` is set) writable.
    bool        user_page_accessible(uint64_t virt, bool write);

    void        invalidate_page(uint64_t virt);

    // Address spaces (one PML4 per process).
    uint64_t    kernel_pml4();
    uint64_t    create_address_space();     // returns PML4 phys addr
    void        destroy_address_space(uint64_t pml4_phys);
    void        switch_address_space(uint64_t pml4_phys);

    // Highest identity-mapped physical address (exclusive).
    uint64_t    identity_limit();
} // namespace paging

#endif // PAGING_H
