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

extern "C" void kmain(BOOT_HEADER *BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack
    memory::setMemorySize(BootHeader->TotalMemorySize);

    gdt::init();
    paging::init((UINT64*)0x300000);
    memory::init();
    idt::init();
    irq::init();
    uart::init(COM1);

    memory::memalloc((UINT64)BootHeader->FrameBufferAddress, 0x600000, BootHeader->FrameBufferSize);

    screen::init(&Screen, BootHeader);
    keyboard::init();
    console::init();
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);

    // list::List<int>* list = list::create<int>();
    // list::add(list, 10);
    // list::add(list, 20);
    // print((UINT64)list, 8);
    // print(list::get(list, 20));

    while (1);
}