#include "../include/boot/boot.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/drivers/console.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/uart.h"
#include "../include/drivers/pit.h"
#include "../include/drivers/rtc.h"
#include "../include/mm/memory.h"
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
    usb::init();

    paging::allocate_pages(0x8000000, (uint64_t)BootHeader->FrameBufferAddress, 
        (BootHeader->FrameBufferSize + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES);

    screen::init(BootHeader);
    keyboard::init();

    pit::init();
    rtc::init();
    irq::install_handler(IRQ0_TIMER, pit::handler);
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);

    pit::calibrate();
    
    console::init();
    
    while (1)
        asm volatile("hlt");
}