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
// Virtual address space layout (higher-half kernel)
// ---------------------------------------------------------------------------
//
// Lower half, PML4[0..255]: owned by the process, different in every address
// space.
//
//   0x0000000000000000  not mapped: the NULL guard (USER_MIN)
//   0x0000000000400000  ELF image, brk heap, mmap window  (see process.h)
//   0x00007FFFFFFFF000  top of the user stack; the last page stays unmapped
//   0x0000800000000000  USER_LIMIT, start of the non-canonical hole
//
// Upper half, PML4[256..511]: the kernel. Built once by init() and copied by
// reference into every address space, so a CR3 switch never changes what the
// kernel sees. Nothing carries PAGE_USER; everything carries PAGE_GLOBAL.
//
//   PML4[256]  0xFFFF800000000000  device window: framebuffer and MMIO BARs,
//                                  2 MiB pages with explicit cache attributes
//   PML4[273]  0xFFFF888000000000  direct map of physical RAM, 2 MiB pages,
//                                  RW + NX. phys_to_virt() lands here.
//   PML4[511]  0xFFFFFFFF80000000  kernel image (-mcmodel=kernel), mapped at
//                                  KERNEL_VMA + phys, executable
//
// Adding a kernel region means claiming a free upper-half PML4 slot and
// populating it in init(): create_address_space() copies the whole upper
// half, so a slot that is empty at that point stays empty in every process.
//
// Physical memory is never dereferenced through its own address. The only
// way from a frame to a pointer is phys_to_virt(); the direct map is the
// reason a page table, a heap chunk or a DMA buffer can live in any frame.

#define USER_MIN              0x10000ULL                  // below: NULL guard
#define USER_LIMIT            0x0000800000000000ULL       // 128 TiB, exclusive
#define USER_PML4_COUNT       256                         // PML4[0..255]

#define DEVICE_PML4_INDEX     256
#define DEVICE_WINDOW_BASE    0xFFFF800000000000ULL
#define DEVICE_WINDOW_SIZE    (4ULL * 1024 * 1024 * 1024) // 4 GiB, 4 static PDs

#define DIRECT_MAP_PML4_INDEX 273
#define DIRECT_MAP_BASE       0xFFFF888000000000ULL
#define DIRECT_MAP_MAX        (32ULL * 1024 * 1024 * 1024) // 32 static PDs

#define KERNEL_PML4_INDEX     511
#define KERNEL_PDPT_INDEX     510
#define KERNEL_VMA            0xFFFFFFFF80000000ULL       // must match linker.ld
#define KERNEL_PHYS_BASE      0x200000ULL                 // where the loader puts it

// Static page tables carved out of the 5 MB the bootloader reserves at
// 0x300000. Keeping them static means device mappings need no allocator and
// can be set up before the PMM exists.
//
// The boot tables are the temporary set kentry.asm builds before it jumps to
// the higher half (identity + direct map + kernel image over the first
// 1 GiB). They are dead once init() loads the final PML4. kentry.asm cannot
// include this header: keep PT_BOOT_* in sync with it by hand.
#define PAGE_TABLES_PHYS      0x300000ULL
#define PT_PML4_OFFSET        0x0000      // PML4
#define PT_DM_PDPT_OFFSET     0x1000      // PDPT for PML4[273]
#define PT_DM_PD_OFFSET       0x2000      // 32 PDs -> 32 GiB of direct map
#define PT_DM_PD_COUNT        32
#define PT_DEV_PDPT_OFFSET    0x22000     // PDPT for PML4[256]
#define PT_DEV_PD_OFFSET      0x23000     // 4 PDs -> 4 GiB of device window
#define PT_DEV_PD_COUNT       4
#define PT_KERN_PDPT_OFFSET   0x27000     // PDPT for PML4[511]
#define PT_KERN_PD_OFFSET     0x28000     // 1 PD -> 1 GiB for the kernel image
#define PT_TOTAL_SIZE         0x29000
#define PT_BOOT_OFFSET        0x30000     // kentry.asm: PML4, PDPT lo, PDPT hi, PD
#define PT_BOOT_SIZE          0x4000

// Physical <-> kernel-virtual translation through the direct map. Valid for
// every frame below paging::direct_map_limit() (the PMM never hands out
// anything else), and - through kentry's boot tables - for the first 1 GiB
// even before paging::init(). Kernel-image statics live at KERNEL_VMA, not in
// the direct map: translate those with paging::virtual_to_phys().
static inline void* phys_to_virt(uint64_t phys)
{
    return (void*)(phys + DIRECT_MAP_BASE);
}

static inline uint64_t virt_to_phys(const void* virt)
{
    return (uint64_t)virt - DIRECT_MAP_BASE;
}

namespace paging
{
    // Build the final kernel half (direct map sized to the memory map, the
    // empty device window, the kernel image) and load CR3. The low identity
    // map kentry.asm needed for the jump is gone afterwards.
    // Run cpu::init_features() first: NX is only set when EFER.NXE is on.
    void        init(uint64_t tables_phys, BOOT_HEADER* boot_header);

    // Map a physical device range into the kernel window and return the
    // virtual address to use. `cache` is PAGE_CACHE_UC or PAGE_CACHE_WC_2M.
    uint64_t    map_device(uint64_t phys, uint64_t size, uint64_t cache);

    // Convenience wrappers: MMIO registers are UC, the framebuffer is WC so
    // that the rep-movsq flush in screen::flush() stays fast.
    uint64_t    map_mmio_region(uint64_t phys, uint64_t size);
    uint64_t    map_framebuffer(uint64_t phys, uint64_t size);

    // 4 KiB mapping in the active address space; intermediate tables come
    // from the PMM. map_page refuses upper-half addresses whose PML4 slot
    // init() left empty (the table would exist in this address space only).
    // map_user_page additionally rejects anything outside
    // [USER_MIN, USER_LIMIT) and forces PAGE_USER, so a malformed ELF can
    // neither map page 0 nor reach into the kernel-shared upper half.
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

    // Deep-copy the lower half of `src_pml4` into `dst_pml4` (a fresh address
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

    // Highest physical address covered by the direct map (exclusive).
    uint64_t    direct_map_limit();
} // namespace paging

#endif // PAGING_H
