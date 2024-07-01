#ifndef KERNEL_H
#define KERNEL_H

#include "./cpu/types.h"

typedef struct {
    // Framebuffer
    void* FrameBufferAddress;
    UINT64 FrameBufferSize;
    UINT32 ScreenWidth;
    UINT32 ScreenHeight;
    UINT32 ScreenPixelsPerScanLine;
    // Console Font
    void* StandartFontBuffer;
    UINT16 FontSymbolSizeX;
    UINT16 FontSymbolSizeY;
    UINT32 FontNumberOfSymbols; // must be <= 256
    // MemoryMap
    void* MemoryMapAddress;
    UINT32 MemoryMapEntrySize;
    UINT64 MemoryMapEntriesNumber;
    UINT64 TotalMemorySize;
    UINT64 FreeMemorySize;
    // Other info about memory
    UINT64 KernelAddress;
    UINT64 KernelSize;
    UINT64 StartDataAddress;
    UINT64 StartDataSize;
} SURFOS_BOOT_HEADER;

typedef struct {
    UINT64 Start;
    UINT64 End;
    UINT32 Type;    // 0 - free memory, 
                    // 1 - my code and data, 
                    // 2 - EFI code and data, 
                    // 3 - ACPI data, 
                    // 4 - reserved and unusable (?), 
                    // 5 - mapped IO,
                    // 6 - other (?)
    UINT32 MemSize;
    UINT64 RESERVED1;
} MEMORY_MAP_ENTRY;

#endif