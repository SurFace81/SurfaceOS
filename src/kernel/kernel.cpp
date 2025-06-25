#include "kernel.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/memory.h"
#include "../include/cpu/paging.h"
#include "../include/drivers/uart.h"
#include "../include/stdlib/stdio.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/console.h"

extern "C" void kmain(BOOT_HEADER *BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack
    memory::setMemorySize(BootHeader->TotalMemorySize);

    gdt::init();
    paging::init((UINT64*)0x300000);
    idt::init();
    irq::init();
    uart::init(COM1);

    memory::memalloc((UINT64)BootHeader->FrameBufferAddress, 0x600000, BootHeader->FrameBufferSize);

    screen::init(&Screen, BootHeader);
    keyboard::init();
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);
    console::init();

    while (1);
}