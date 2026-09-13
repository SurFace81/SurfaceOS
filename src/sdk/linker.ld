OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS {
    /* USER_IMAGE_VADDR: USER_BASE + 1 MB (see kernel process.h) */
    . = 0x40000100000;
    .text : {
        *(.text._start)
        *(.text)
        *(.rodata*)
    }
    .data ALIGN(16) : {
        *(.data)
    }
    .bss ALIGN(16) : {
        *(.bss)
    }
}
