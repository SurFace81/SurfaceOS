#include "kernel.h"
#include "../include/cpu/gdt.h"
#include "../include/mm/memory.h"
#include "../include/cpu/paging.h"
#include "../include/drivers/uart.h"
#include "../include/stdlib/stdio.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/console.h"
#include "../include/stdlib/list.h"
#include "../include/cpu/pci.h"

extern "C" void kmain(BOOT_HEADER *BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack
    memory::setMemorySize(BootHeader->TotalMemorySize);

    gdt::init();
    paging::init((uint64_t*)0x300000);
    memory::init();
    idt::init();
    irq::init();
    pci::init();
    uart::init();

    memory::memalloc((uint64_t)BootHeader->FrameBufferAddress, 0x600000, BootHeader->FrameBufferSize);

    screen::init(&Screen, BootHeader);
    keyboard::init();
    console::init();
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);

    uart::listen_loop();
    while (1);
}