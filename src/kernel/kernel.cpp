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
#include "../include/drivers/fs/fat32.h"

// Console command handlers (before kmain or in a separate file)
static void cmd_mount(list::List<char>*)
{
    if (fat32::mount(0))
        screen::printf("\n\rFAT32 mounted\n\r");
    else
        screen::printf("\n\rMount failed\n\r");
}

static void cmd_ls(list::List<char>*)
{
    screen::printf("\n\r");
    fat32::ls("/");
}

static void cmd_cat(list::List<char>* args)
{
    // For now just hardcode a test — later parse args
    // Example: read first 1024 bytes of a file
    uint8_t buf[1024];
    uint32_t n = fat32::read_file("TEST.TXT", buf, 1023);
    screen::printf("\n\r");
    if (n > 0)
    {
        buf[n] = 0;
        screen::printf("%s", (char*)buf);
    }
    else
    {
        screen::printf("File not found or read error\n\r");
    }
}

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

    usb::init();

    console::register_command("mount", cmd_mount);
    console::register_command("ls", cmd_ls);
    console::register_command("cat", cmd_cat);
    
    while (1);
}