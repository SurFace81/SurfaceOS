#ifndef ELF_H
#define ELF_H

#include "types.h"
#include "../fs/vfs.h"

// ELF64 header
struct Elf64_Ehdr
{
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

// ELF64 program header
struct Elf64_Phdr
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

// e_ident indices
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5

// e_ident values
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1

// e_type
#define ET_EXEC     2

// e_machine
#define EM_X86_64   62

// p_type
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4

// p_flags
#define PF_X        1
#define PF_W        2
#define PF_R        4

namespace elf
{
    struct LoadResult
    {
        uint64_t entry;         // entry point virtual address
        uint64_t image_end;     // highest mapped vaddr (page-aligned)

        // Where the program header table ended up in the image, for AT_PHDR.
        // phdr_vaddr == 0 means no PT_LOAD segment covers e_phoff (a static
        // executable that does not map its own phdrs - musl tolerates that
        // by ignoring AT_PHDR, but our SDK images always include it).
        uint64_t phdr_vaddr;
        uint64_t phdr_entsize;
        uint64_t phdr_num;

        bool     valid;
    };

    // Check if a buffer starts with a valid ELF64 header.
    bool is_elf(const uint8_t* data, uint32_t size);

    // Load PT_LOAD segments of a regular-file vnode into the current address
    // space, reading each segment's file-backed part by p_offset in chunks:
    // the file is never copied whole. Pages come from the PMM and are mapped
    // with permissions from p_flags; BSS (p_memsz > p_filesz) is zeroed by
    // the page allocator. Returns valid=false with rc<0 on any error;
    // *out_rc receives the -errno when invalid.
    LoadResult load_file(vnode* v, uint64_t file_size, sint64_t* out_rc);
}

#endif // ELF_H
