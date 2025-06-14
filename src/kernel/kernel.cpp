#include "kernel.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/memory.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/cpu/uart.h"
#include "../include/drivers/init/screen_init.h"
#include "../include/stdlib/stdio.h"
#include "../include/cpu/idt.h"

extern "C" void kmain(SFOS_BOOT_HEADER *BootHeader)
{
    asm volatile("movq $0x200000, %rsp"); // move stack
    setMemorySize(BootHeader->TotalMemorySize);

    initGDT();
    initPaging((void *)0x300000);
    init_idt();
    initUART(COM1);

    memalloc((UINT64)BootHeader->FrameBufferAddress, 0x600000, BootHeader->FrameBufferSize);

    initScreen(&Screen, BootHeader);

    for (unsigned int i = 0; i < 128; i++) {
        char buffer[2];
        buffer[0] = (char)i;
        buffer[1] = '\0';
        print(buffer);

        if ((i + 1) % 16 == 0) {
            print("\n\r");
        } else {
            print(" ");
        }
    }

    //int a = 10 / 0;

    //print("\n#DE Exception\n");

    asm volatile("int $16");
    asm volatile("int $0");
    asm volatile("int $7");
    asm volatile("int $2");

    print("\nINTs were be sended!");

    while (1);
}