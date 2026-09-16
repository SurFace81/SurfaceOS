OUTPUT_FORMAT("binary")
ENTRY(_start)

/* Physical layout of the low memory the kernel owns (see also mm/pmm.cpp):
 *
 *   0x004000            font.fnt            (loaded by the bootloader)
 *   0x100000 - 0x108000 StartData           (boot header + memory map)
 *   0x200000 - __kernel_end                 kernel image + .bss
 *   0x300000 - 0x327000 static page tables  (AllocatePages'd by the bootloader)
 *
 * The ASSERT below turns a kernel that grew into the page tables into a
 * build failure instead of memory corruption that only shows up at runtime.
 */
KERNEL_PHYS_BASE  = 0x200000;
PAGE_TABLES_BASE  = 0x300000;

SECTIONS {
	. = KERNEL_PHYS_BASE;

	.text : {
		*(.text.boot)
		*(.text .text.*)
		*(.gnu.linkonce.t.*)
		*(.code)
	}

	.rodata ALIGN(0x1000) : {
		*(.rodata .rodata.*)
		*(.gnu.linkonce.r.*)
	}

	.data ALIGN(0x1000) : {
		*(.data .data.*)
		*(.gnu.linkonce.d.*)
	}

	/* .bss is deliberately NOLOAD: it contributes no bytes to the flat
	 * binary, and the UEFI loader copies only the bytes that are in the
	 * file. kentry.asm zeroes [__bss_start, __bss_end) before calling
	 * kmain -- without that every uninitialised kernel static (idt[],
	 * irq_handlers[], console pending_valid, ...) starts with whatever
	 * the firmware left in RAM. QEMU hands out zeroed pages and hides
	 * the bug; real firmware does not. */
	.bss ALIGN(0x1000) (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		*(.gnu.linkonce.b.*)
		. = ALIGN(8);
		__bss_end = .;
	}

	__kernel_end = .;

	/DISCARD/ : {
		*(.eh_frame)
		*(.eh_frame_hdr)
		*(.note*)
		*(.comment)
	}
}

ASSERT(__kernel_end <= PAGE_TABLES_BASE,
       "kernel image + .bss grew past 0x300000 and now overlaps the static page tables")
