OUTPUT_FORMAT("binary")
ENTRY(_start)

/* Higher-half kernel. The loader copies kernel.bin to physical 0x200000 and
 * jumps to its first byte; the kernel runs at KERNEL_VMA + phys.
 *
 *   .boot           VMA = LMA = 0x200000. kentry.asm's _start: builds the
 *                   boot page tables and jumps to the higher half. Must be
 *                   first in the file.
 *   .text .. .bss   VMA = KERNEL_VMA + LMA, packed right behind .boot. The
 *                   flat binary is laid out by LMA, so the file still starts
 *                   at 0x200000 and the loader needs no changes.
 *
 * Physical layout of the low memory the kernel owns (see also mm/pmm.cpp):
 *
 *   0x004000            font.fnt            (loaded by the bootloader)
 *   0x100000 - 0x108000 StartData           (boot header + memory map)
 *   0x200000 - __kernel_phys_end            kernel image + .bss
 *   0x300000 - 0x334000 static + boot page tables (AllocatePages'd by the
 *                       bootloader; layout in include/cpu/paging.h)
 *
 * The ASSERT below turns a kernel that grew into the page tables into a
 * build failure instead of memory corruption that only shows up at runtime.
 */
KERNEL_PHYS_BASE  = 0x200000;
KERNEL_VMA        = 0xFFFFFFFF80000000;   /* KERNEL_VMA in cpu/paging.h */
PAGE_TABLES_BASE  = 0x300000;

SECTIONS {
	. = KERNEL_PHYS_BASE;

	.boot : {
		*(.text.boot)
	}

	. += KERNEL_VMA;

	.text ALIGN(0x1000) : AT(ADDR(.text) - KERNEL_VMA) {
		*(.text .text.*)
		*(.gnu.linkonce.t.*)
		*(.code)
	}

	.rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KERNEL_VMA) {
		*(.rodata .rodata.*)
		*(.gnu.linkonce.r.*)
	}

	.data ALIGN(0x1000) : AT(ADDR(.data) - KERNEL_VMA) {
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
	.bss ALIGN(0x1000) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		*(.gnu.linkonce.b.*)
		. = ALIGN(8);
		__bss_end = .;
	}

	__kernel_end      = .;
	__kernel_phys_end = . - KERNEL_VMA;

	/DISCARD/ : {
		*(.eh_frame)
		*(.eh_frame_hdr)
		*(.note*)
		*(.comment)
	}
}

ASSERT(__kernel_phys_end <= PAGE_TABLES_BASE,
       "kernel image + .bss grew past 0x300000 and now overlaps the static page tables")
