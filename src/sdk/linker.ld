OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* One PT_LOAD per permission class, declared explicitly. Left to itself ld
 * folds .rodata into the R+X code segment, which would make every constant
 * in the app executable. The kernel's ELF loader turns p_flags straight into
 * page permissions (no PF_X -> PAGE_NX, no PF_W -> read-only), so these
 * headers are what gives an app W^X.
 *
 * FILEHDR PHDRS on the first segment includes the ELF header and the phdr
 * table in it at file offset 0 (the Linux layout), which is what makes
 * AT_PHDR point at real memory in the loaded image.
 *
 * The link address is USER_IMAGE_VADDR (src/include/cpu/process.h); the
 * image must end below USER_MMAP_BASE. Pages below 0x10000 are the NULL
 * guard and never mapped.
 */
PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);    /* R X */
    rodata PT_LOAD FLAGS(4);                  /* R   */
    data   PT_LOAD FLAGS(6);                  /* R W */
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text._start)
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } :text

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    } :rodata

    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    } :data

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
        *(.gnu.linkonce.b.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note*)
        *(.comment)
    }
}
