OUTPUT_FORMAT("elf64-x86-64")
ENTRY(SfMain)

/* A SurfaceOS program. No libc and no start-up code of its own: the kernel
 * starts it through the SDK code page, which calls SfMain (the ELF entry)
 * with App and Sys and ends the process with what it returns.
 *
 * One PT_LOAD per permission class: the kernel's ELF loader turns p_flags
 * straight into page permissions (no PF_X -> NX, no PF_W -> read-only), so
 * this split is what gives a program W^X. FILEHDR PHDRS puts the ELF
 * header in the first segment at file offset 0.
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
        *(.text .text.*)
    } :text

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note*)
        *(.comment)
    }
}
