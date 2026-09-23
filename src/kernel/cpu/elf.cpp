// ELF64 loader: maps PT_LOAD segments of a vnode into the current address
// space with per-segment permissions.
//
// The image is untrusted input: every header field is range-checked before
// it is used, and every mapping goes through paging::map_user_page so a
// segment can only ever land inside the process's own PML4 entry.
//
// Segments are read from the file by p_offset in page-sized chunks straight
// into their destination physical frames - the executable is never copied
// whole into kernel memory (USER_IMAGE_MAX is 64 MiB, a kmalloc of that on
// a 128 MiB machine would be reckless). The header and phdr table are small
// and are read into scratch buffers.

#include "../../include/cpu/elf.h"
#include "../../include/cpu/paging.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"

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

        memory::memset((uint8_t*)phys_to_virt(frame), 0x00, PAGE_SIZE_4K);

        if (!paging::map_user_page(vaddr, frame, flags))
        {
            pmm::free_frame(frame);
            return false;
        }
        return true;
    }

    // Read `len` bytes of the file at `off` into `dst`. Returns the number
    // of bytes read (short reads are legal near EOF), or -errno.
    static sint64_t read_at(vnode* v, uint64_t off, void* dst, uint64_t len)
    {
        uint64_t done = 0;
        sint64_t rc = v->ops->read(v, off, dst, len, &done);
        if (rc != 0)
            return rc;
        return (sint64_t)done;
    }

    LoadResult load_file(vnode* v, uint64_t file_size, sint64_t* out_rc)
    {
        LoadResult result = {0, 0, 0, 0, 0, false};

        if (file_size < sizeof(Elf64_Ehdr))
        {
            *out_rc = -ENOEXEC;
            return result;
        }

        // Header into a scratch buffer.
        Elf64_Ehdr* ehdr = (Elf64_Ehdr*)kmalloc(sizeof(Elf64_Ehdr));
        if (!ehdr)
        {
            *out_rc = -ENOMEM;
            return result;
        }

        sint64_t got = read_at(v, 0, ehdr, sizeof(Elf64_Ehdr));
        if (got != (sint64_t)sizeof(Elf64_Ehdr))
        {
            kfree(ehdr);
            *out_rc = got < 0 ? (sint64_t)got : -EIO;
            return result;
        }

        if (!is_elf((const uint8_t*)ehdr, sizeof(Elf64_Ehdr)))
        {
            kfree(ehdr);
            *out_rc = -ENOEXEC;
            return result;
        }

        if (ehdr->e_phentsize < sizeof(Elf64_Phdr) || ehdr->e_phnum == 0 ||
            ehdr->e_phnum > 1024)
        {
            kfree(ehdr);
            *out_rc = -ENOEXEC;
            return result;
        }
        uint64_t ph_bytes = (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
        if (ehdr->e_phoff + ph_bytes > file_size)
        {
            kfree(ehdr);
            *out_rc = -ENOEXEC;
            return result;
        }

        // Program header table into scratch.
        Elf64_Phdr* phdrs = (Elf64_Phdr*)kmalloc(ph_bytes);
        if (!phdrs)
        {
            kfree(ehdr);
            *out_rc = -ENOMEM;
            return result;
        }
        got = read_at(v, ehdr->e_phoff, phdrs, ph_bytes);
        if (got != (sint64_t)ph_bytes)
        {
            kfree(phdrs);
            kfree(ehdr);
            *out_rc = got < 0 ? (sint64_t)got : -EIO;
            return result;
        }

        // One chunk buffer for copying file-backed segment slices into their
        // destination pages.
        uint8_t* chunk = (uint8_t*)kmalloc(PAGE_SIZE_4K);
        if (!chunk)
        {
            kfree(phdrs);
            kfree(ehdr);
            *out_rc = -ENOMEM;
            return result;
        }

        uint64_t image_end = 0;
        uint64_t phdr_vaddr = 0;

        for (uint16_t i = 0; i < ehdr->e_phnum; i++)
        {
            const Elf64_Phdr* phdr =
                (const Elf64_Phdr*)((const uint8_t*)phdrs + (uint64_t)i * ehdr->e_phentsize);

            if (phdr->p_type != PT_LOAD)
                continue;

            if (phdr->p_filesz > phdr->p_memsz)
            {
                *out_rc = -ENOEXEC;
                goto fail;
            }
            // Bounds-check only the file-backed part. A pure-BSS segment
            // (filesz == 0) may have p_offset beyond the end of the file.
            if (phdr->p_filesz > 0 && phdr->p_offset + phdr->p_filesz > file_size)
            {
                *out_rc = -ENOEXEC;
                goto fail;
            }
            if (!paging::is_user_range(phdr->p_vaddr, phdr->p_memsz))
            {
                uart::printf("elf: segment %u at %llx is outside user space\n",
                             (uint32_t)i, phdr->p_vaddr);
                *out_rc = -ENOEXEC;
                goto fail;
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
                {
                    *out_rc = -ENOMEM;
                    goto fail;
                }

                // Copy the file-backed slice that falls inside *this* page,
                // reading it from the file in at most one page at a time.
                uint64_t page_end   = page + PAGE_SIZE_4K;
                uint64_t copy_start = (page > seg_start) ? page : seg_start;
                uint64_t copy_end   = (page_end < seg_file_end) ? page_end : seg_file_end;

                if (copy_end <= copy_start)
                    continue;               // BSS-only part: page already zeroed

                uint64_t src_off  = phdr->p_offset + (copy_start - phdr->p_vaddr);
                uint64_t copy_len = copy_end - copy_start;
                uint64_t dst_phys = paging::virtual_to_phys(copy_start);
                if (!dst_phys)
                {
                    *out_rc = -EFAULT;
                    goto fail;
                }

                sint64_t rd = read_at(v, src_off, chunk, copy_len);
                if (rd != (sint64_t)copy_len)
                {
                    *out_rc = rd < 0 ? (sint64_t)rd : -EIO;
                    goto fail;
                }
                // Written through the direct map, i.e. as supervisor memory:
                // deliberately not a uaccess copy.
                memory::memcpy((uint8_t*)phys_to_virt(dst_phys), chunk, copy_len);
            }

            if (seg_end > image_end)
                image_end = seg_end;
        }

        if (!image_end)
        {
            *out_rc = -ENOEXEC;             // no loadable segment
            goto fail;
        }

        // The entry point has to be inside something we actually mapped.
        if (ehdr->e_entry < USER_MIN || ehdr->e_entry >= image_end)
        {
            *out_rc = -ENOEXEC;
            goto fail;
        }

        // AT_PHDR: does a PT_LOAD segment carry the program header table?
        for (uint16_t i = 0; i < ehdr->e_phnum; i++)
        {
            const Elf64_Phdr* phdr =
                (const Elf64_Phdr*)((const uint8_t*)phdrs + (uint64_t)i * ehdr->e_phentsize);
            if (phdr->p_type != PT_LOAD)
                continue;
            if (ehdr->e_phoff >= phdr->p_offset &&
                ehdr->e_phoff + ph_bytes <= phdr->p_offset + phdr->p_filesz)
            {
                phdr_vaddr = phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);
                break;
            }
        }

        result.entry        = ehdr->e_entry;
        result.image_end    = (image_end + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        result.phdr_vaddr   = phdr_vaddr;
        result.phdr_entsize = ehdr->e_phentsize;
        result.phdr_num     = ehdr->e_phnum;
        result.valid        = true;
        *out_rc = 0;

        kfree(chunk);
        kfree(phdrs);
        kfree(ehdr);
        return result;

    fail:
        kfree(chunk);
        kfree(phdrs);
        kfree(ehdr);
        return result;      // valid == false, *out_rc set
    }
} // namespace elf
