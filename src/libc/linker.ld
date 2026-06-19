OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS {
    .text 0x3000000 : {
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