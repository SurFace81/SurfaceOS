#include "kernel.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/memory.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/cpu/uart.h"
#include "../include/drivers/init/screen_init.h"
#include "../include/stdlib/stdio.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"

extern "C" void timer_handler(void) {
    UINT8 scan_code = port_byte_in(0x60);

    if (scan_code < 0x80) {
        print_str(".");
    }
}

extern "C" void kmain(SFOS_BOOT_HEADER *BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack
    setMemorySize(BootHeader->TotalMemorySize);

    initGDT();
    initPaging((void *)0x300000);
    initIDT();
    initIRQ();
    initUART(COM1);

    memalloc((UINT64)BootHeader->FrameBufferAddress, 0x600000, BootHeader->FrameBufferSize);

    initScreen(&Screen, BootHeader);

    for (unsigned int i = 0; i < 128; i++) {
        char buffer[2];
        buffer[0] = (char)i;
        buffer[1] = '\0';
        print_str(buffer);

        if ((i + 1) % 16 == 0) {
            print_str("\n\r");
        } else {
            print_str(" ");
        }
    }

    irq_install_handler(IRQ1_KEYBOARD, timer_handler);

    while (1);
}