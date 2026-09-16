OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* One PT_LOAD per permission class, declared explicitly. Left to itself ld
 * folds .rodata into the R+X code segment, which would make every constant
 * in the app executable. The kernel's ELF loader turns p_flags straight into
 * page permissions (no PF_X -> PAGE_NX, no PF_W -> read-only), so these
 * headers are what gives an app W^X.
 *
 * The link address must stay inside [USER_BASE, USER_MMAP_BASE); see
 * USER_IMAGE_VADDR in src/include/cpu/process.h.
 */
PHDRS
{
    text   PT_LOAD FLAGS(5);    /* R X */
    rodata PT_LOAD FLAGS(4);    /* R   */
    data   PT_LOAD FLAGS(6);    /* R W */
}

SECTIONS {
    . = 0x40000100000;

    .text : ALIGN(0x1000) {
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
