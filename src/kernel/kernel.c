#include "kernel.h"
#include "./drivers/init/screen_init.h"
#include "./stdlib/stdio.h"
#include "./stdlib/string.h"
#include "./cpu/gdt.h"
#include "./cpu/memory.h"
#include "./cpu/paging.h"


void testPrint(SURFOS_BOOT_HEADER* BootHeader) {
    printf("Memory info:\t");
    printi(getMemorySize());
    printf("\t");
    printi((UINT64)BootHeader->FreeMemorySize / 1024);
    printf(" KB\t");
    printi((UINT64)BootHeader->MemoryMapEntriesNumber);
    printf("\n\n\r");

    printf("Memory Map:\n\r");
    MEMORY_MAP_ENTRY* entry = (MEMORY_MAP_ENTRY*)BootHeader->MemoryMapAddress;
    for (unsigned int i = 1; i <= BootHeader->MemoryMapEntriesNumber; i++) {
        printh((UINT64)entry->Start, 16);
        printf(" - ");
        printh((UINT64)entry->End, 16);
        printf("\tSize: ");
        printi((UINT64)entry->MemSize / 1024);
        printf(" KB\tType: ");
        printi((UINT32)entry->Type);
        printf("\n\r");

        entry = (MEMORY_MAP_ENTRY*)(BootHeader->MemoryMapAddress + i * BootHeader->MemoryMapEntrySize);
    }
    printf("\n\r");

    printf("FrameBuffer info:\t");
    printh((UINT64)BootHeader->FrameBufferAddress, 16);
    printf(" ");
    printh((UINT64)BootHeader->FrameBufferSize, 16);
    printf(" ");
    printi((int)BootHeader->ScreenHeight);
    printf(" ");
    printi((int)BootHeader->ScreenWidth);
    printf(" ");
    printh((UINT64)BootHeader->StandartFontBuffer, 16);
}

void kmain(SURFOS_BOOT_HEADER* BootHeader) {
    setMemorySize(BootHeader->TotalMemorySize);
    
    // init CPU
    initGDT();
    //initPaging((void*)0x300000);
    
    // init Hardware
    initScreen(&Screen, BootHeader);

    testPrint(BootHeader);
}