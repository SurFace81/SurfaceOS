OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS {
    /* USER_IMAGE_VADDR: USER_BASE + 1 MB (see kernel process.h) */
    . = 0x40000100000;

    .text : ALIGN(0x1000) {
        *(.text._start)
        *(.text)
        *(.rodata*)
    }

    .data : ALIGN(0x1000) {
        *(.data)
    }

    .bss : ALIGN(0x1000) {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}
