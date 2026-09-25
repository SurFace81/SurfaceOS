OUTPUT_FORMAT("elf64-x86-64")
ENTRY(SfMain)

/* A SurfaceOS program. No libc and no start-up code of its own: the kernel
 * starts it through the SDK code page, which calls SfMain (the ELF entry)
 * with App and Sys and ends the process with what it returns.
 *
 * The segments are the same W^X split as for the old SDK (linker.ld), plus
 * a PT_NOTE carrying .note.sfos (sfos/note.S): that note is what marks the
 * file as a SurfaceOS program.
 */
PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);    /* R X */
    rodata PT_LOAD FLAGS(4);                  /* R   */
    data   PT_LOAD FLAGS(6);                  /* R W */
    note   PT_NOTE FLAGS(4);
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    .note.sfos : {
        KEEP(*(.note.sfos))
    } :rodata :note

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
        *(.note.*)
        *(.comment)
    }
}
