#include "kernel.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/drivers/console.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/uart.h"
#include "../include/mm/memory.h"
#include "../include/stdlib/list.h"
#include "../include/stdlib/stdio.h"
#include "../include/drivers/usb/xhci.h"

extern "C" void kmain(BOOT_HEADER* BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack

    gdt::init();
    paging::init((uint64_t*)0x300000);
    memory::init(BootHeader->TotalMemorySize);
    idt::init();
    irq::init();
    pci::init();
    uart::init();

    paging::allocate_pages(0x600000, (uint64_t)BootHeader->FrameBufferAddress, 
        (BootHeader->FrameBufferSize + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES);

    screen::init(BootHeader);
    keyboard::init();
    console::init();
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);

    xhci::init();

    //uart::listen_loop();
    while (1);
}