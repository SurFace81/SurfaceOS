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

void keyb(keyboard_event_t e) {
    if (e.type == KEY_PRESS) {
        print("\n\r");

        if (e.modifiers & MOD_ALT) {
            print("Alt + ");
        }
        if (e.modifiers & MOD_SHIFT) {
            print("Shift + ");
        }
        if (e.modifiers & MOD_CAPS) {
            print("Caps + ");
        }
        if (e.modifiers & MOD_CTRL) {
            print("Ctrl + ");
        }
        if (e.modifiers & MOD_NUM) {
            print("Num + ");
        }
        if (e.modifiers & MOD_SCROLL) {
            print("Scroll + ");
        }

        print(e.keyCode);
        char out[3] = {' ', e.key, '\0'};
        print(out);
    }
}

extern "C" void kmain(SFOS_BOOT_HEADER *BootHeader)
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
    print("\n\tSurfaceOS v0.1 (C) 2025\n\r");
    print("\tMem: ");
    print(memory::getMemorySize() / 1048576 + 1);
    print(" Mb");
    print("\n\r------------------------------------------------\n\n\r> ");

    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);

    keyboard::set_keyboard_callback(keyb);

    while (1);
}