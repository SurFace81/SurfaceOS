// ELF64 loader: maps PT_LOAD segments of an executable into the current
// address space with per-segment permissions.

#include "../../include/cpu/elf.h"
#include "../../include/cpu/paging.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"

namespace elf
{
    bool is_elf(const uint8_t* data, uint32_t size)
    {
        if (size < sizeof(Elf64_Ehdr))
            return false;

        const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;

        if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
            ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
            ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
            ehdr->e_ident[EI_MAG3] != ELFMAG3)
            return false;

        if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
            return false;

        if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
            return false;

        if (ehdr->e_type != ET_EXEC)
            return false;

        if (ehdr->e_machine != EM_X86_64)
            return false;

        return true;
    }

    // Map a single page with the given flags, allocating a fresh zeroed frame.
    // If the page is already mapped (two segments sharing a page), keep the
    // existing mapping and just upgrade its permissions.
    static bool map_zero_page(uint64_t vaddr, uint64_t flags)
    {
        uint64_t existing = paging::virtual_to_phys(vaddr);
        if (existing)
        {
            // Page already mapped: merge in any new permission bits.
            paging::upgrade_page_flags(vaddr, flags);
            return true;
        }

        uint64_t frame = pmm::alloc_frame();
        if (!frame)
            return false;

        memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
        return paging::map_page(vaddr, frame, flags);
    }

    LoadResult load(const uint8_t* image, uint32_t image_size)
    {
        LoadResult result = {0, 0, false};

        if (!is_elf(image, image_size))
            return result;

        const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)image;

        if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > image_size)
            return result;

        uint64_t image_end = 0;

        for (uint16_t i = 0; i < ehdr->e_phnum; i++)
        {
            const Elf64_Phdr* phdr =
                (const Elf64_Phdr*)(image + ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize);

            if (phdr->p_type != PT_LOAD)
                continue;

            if (phdr->p_offset + phdr->p_filesz > image_size)
                return result;

            // Build page flags from segment permissions
            uint64_t flags = PAGE_USER;
            if (phdr->p_flags & PF_W)
                flags |= PAGE_WRITE;
            // Execute permission is implicit (no NX bit set)

            uint64_t seg_start = phdr->p_vaddr;
            uint64_t seg_end   = phdr->p_vaddr + phdr->p_memsz;

            uint64_t first_page = seg_start & ~(PAGE_SIZE_4K - 1);
            uint64_t last_page  = (seg_end + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);

            for (uint64_t page = first_page; page < last_page; page += PAGE_SIZE_4K)
            {
                if (!map_zero_page(page, flags))
                    return result;

                // Copy the file-backed portion of this page
                uint64_t page_off_in_seg = (page > seg_start) ? (page - seg_start) : 0;
                uint64_t copy_start = seg_start + page_off_in_seg;
                uint64_t copy_len = PAGE_SIZE_4K;

                if (copy_start < seg_start)
                    copy_start = seg_start;

                uint64_t seg_file_end = phdr->p_vaddr + phdr->p_filesz;
                if (copy_start < seg_file_end)
                {
                    uint64_t avail = seg_file_end - copy_start;
                    if (avail < copy_len)
                        copy_len = avail;

                    uint64_t src_off = phdr->p_offset + (copy_start - phdr->p_vaddr);
                    uint64_t dst_phys = paging::virtual_to_phys(copy_start);

                    if (dst_phys)
                        memory::memcpy((uint8_t*)dst_phys, (uint8_t*)image + src_off, copy_len);
                }
            }

            if (seg_end > image_end)
                image_end = seg_end;
        }

        result.entry = ehdr->e_entry;
        result.image_end = (image_end + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        result.valid = true;
        return result;
    }
} // namespace elf
