/* The SDK runtime (abi/sdkimage.h): code and constant tables at
   SDK_CODE_ADDRESS, variables at SDK_STATE_ADDRESS. The kernel copies the
   code part in (objcopy -j .text) and gives every process zeroed pages
   for the variables, so the runtime may not have initialised ones. */

SECTIONS
{
    . = 0x00007FFF00000000;             /* SDK_CODE_ADDRESS */

    .text :
    {
        KEEP(*(.sdk_header))            /* SdkHeader comes first */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data.rel.ro .data.rel.ro.*)
    }
    ASSERT(SIZEOF(.text) <= 0x100000, "the SDK runtime is larger than SDK_CODE_MAX")

    . = 0x00007FFF00200000;             /* SDK_STATE_ADDRESS */

    .data :
    {
        *(.data .data.*)
    }
    ASSERT(SIZEOF(.data) == 0, "the SDK runtime may not have initialised variables")

    .bss (NOLOAD) :
    {
        SdkStateStart = .;
        *(.bss .bss.*)
        *(COMMON)
        SdkStateEnd = .;
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
