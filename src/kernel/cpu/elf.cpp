// ELF64 loader: maps PT_LOAD segments of an executable into the current
// address space with per-segment permissions.
//
// The image is untrusted input: every header field is range-checked before
// it is used, and every mapping goes through paging::map_user_page so a
// segment can only ever land inside the process's own PML4 entry.

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

    // Map a single user page with the given flags, allocating a fresh zeroed
    // frame. If the page is already mapped (two segments sharing a page),
    // keep the existing frame and merge the permissions.
    static bool map_zero_page(uint64_t vaddr, uint64_t flags)
    {
        if (paging::virtual_to_phys(vaddr))
        {
            paging::upgrade_page_flags(vaddr, flags);
            return true;
        }

        uint64_t frame = pmm::alloc_frame();
        if (!frame)
            return false;

        memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);

        if (!paging::map_user_page(vaddr, frame, flags))
        {
            pmm::free_frame(frame);
            return false;
        }
        return true;
    }

    LoadResult load(const uint8_t* image, uint32_t image_size)
    {
        LoadResult result = {0, 0, false};

        if (!is_elf(image, image_size))
            return result;

        const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)image;

        if (ehdr->e_phentsize < sizeof(Elf64_Phdr))
            return result;
        if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > image_size)
            return result;

        uint64_t image_end = 0;

        for (uint16_t i = 0; i < ehdr->e_phnum; i++)
        {
            const Elf64_Phdr* phdr =
                (const Elf64_Phdr*)(image + ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize);

            if (phdr->p_type != PT_LOAD)
                continue;

            if (phdr->p_filesz > phdr->p_memsz)
                return result;

            // Bounds-check only the file-backed part. A pure-BSS segment
            // (filesz == 0) may have p_offset beyond the end of the file;
            // that is valid and must not be rejected.
            if (phdr->p_filesz > 0 && phdr->p_offset + phdr->p_filesz > image_size)
                return result;

            // The segment must fit inside this process's own address space.
            // Without this, an ELF asking for p_vaddr in the low canonical
            // range would have the loader walk PML4[0] - shared with the
            // kernel - and map user-writable pages into the kernel's tables.
            if (!paging::is_user_range(phdr->p_vaddr, phdr->p_memsz))
            {
                uart::printf("elf: segment %u at %llx is outside user space\n",
                             (uint32_t)i, phdr->p_vaddr);
                return result;
            }

            uint64_t flags = 0;
            if (phdr->p_flags & PF_W)
                flags |= PAGE_WRITE;
            if (!(phdr->p_flags & PF_X))
                flags |= PAGE_NX;

            uint64_t seg_start    = phdr->p_vaddr;
            uint64_t seg_end      = phdr->p_vaddr + phdr->p_memsz;
            uint64_t seg_file_end = phdr->p_vaddr + phdr->p_filesz;

            uint64_t first_page = seg_start & ~(PAGE_SIZE_4K - 1);
            uint64_t last_page  = (seg_end + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);

            for (uint64_t page = first_page; page < last_page; page += PAGE_SIZE_4K)
            {
                if (!map_zero_page(page, flags))
                    return result;

                // Copy the file-backed slice that falls inside *this* page.
                // Clamping to the page end is what keeps a segment whose
                // p_vaddr is not page-aligned - the normal case for anything
                // not linked by our own SDK script - from running the memcpy
                // off the end of the frame and into an unrelated one.
                uint64_t page_end   = page + PAGE_SIZE_4K;
                uint64_t copy_start = (page > seg_start) ? page : seg_start;
                uint64_t copy_end   = (page_end < seg_file_end) ? page_end : seg_file_end;

                if (copy_end <= copy_start)
                    continue;

                uint64_t src_off  = phdr->p_offset + (copy_start - phdr->p_vaddr);
                uint64_t dst_phys = paging::virtual_to_phys(copy_start);
                if (!dst_phys)
                    return result;

                // Written through the identity map, i.e. as supervisor memory:
                // this is deliberately not a uaccess copy.
                memory::memcpy((uint8_t*)dst_phys, (uint8_t*)image + src_off,
                               copy_end - copy_start);
            }

            if (seg_end > image_end)
                image_end = seg_end;
        }

        if (!image_end)
            return result;              // no loadable segment

        // The entry point has to be inside something we actually mapped.
        if (ehdr->e_entry < USER_BASE || ehdr->e_entry >= image_end)
            return result;

        result.entry = ehdr->e_entry;
        result.image_end = (image_end + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        result.valid = true;
        return result;
    }
} // namespace elf
