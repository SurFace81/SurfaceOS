#include "kernel.h"
#include "./drivers/init/screen_init.h"
#include "./stdlib/stdio.h"
#include "./stdlib/string.h"
#include "./cpu/gdt.h"
#include "./cpu/memory.h"
#include "./cpu/paging.h"


void testPrint(SURFOS_BOOT_HEADER* BootHeader) {
    print("Memory info:\t");
    print(getMemorySize());
    print("\t");
    print((UINT64)BootHeader->FreeMemorySize / 1024);
    print(" KB\t");
    print((UINT64)BootHeader->MemoryMapEntriesNumber);
    print("\n\n\r");

    print("Memory Map:\n\r");
    MEMORY_MAP_ENTRY* entry = (MEMORY_MAP_ENTRY*)BootHeader->MemoryMapAddress;
    for (unsigned int i = 1; i <= BootHeader->MemoryMapEntriesNumber; i++) {
        print((UINT64)entry->Start, 16);
        print(" - ");
        print((UINT64)entry->End, 16);
        print("\tSize: ");
        print((UINT64)entry->MemSize / 1024);
        print(" KB\tType: ");
        print((UINT32)entry->Type);
        print("\n\r");

        entry = (MEMORY_MAP_ENTRY*)(BootHeader->MemoryMapAddress + i * BootHeader->MemoryMapEntrySize);
    }
    print("\n\r");

    print("FrameBuffer info:\t");
    print((UINT64)BootHeader->FrameBufferAddress, 16);
    print(" ");
    print((UINT64)BootHeader->FrameBufferSize, 16);
    print(" ");
    print((int)BootHeader->ScreenHeight);
    print(" ");
    print((int)BootHeader->ScreenWidth);
    print(" ");
    print((UINT64)BootHeader->StandartFontBuffer, 16);
}

extern "C" void kmain(SURFOS_BOOT_HEADER* BootHeader) {
    setMemorySize(BootHeader->TotalMemorySize);
    
    // init CPU
    initGDT();
    //initPaging((void*)0x300000);
    
    // init Hardware
    initScreen(&Screen, BootHeader);

    testPrint(BootHeader);
}