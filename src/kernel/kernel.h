#ifndef KERNEL_H
#define KERNEL_H

#include "../include/cpu/types.h"

typedef struct {
    // Framebuffer
    void* FrameBufferAddress;
    uint64_t FrameBufferSize;
    uint32_t ScreenWidth;
    uint32_t ScreenHeight;
    uint32_t ScreenPixelsPerScanLine;
    // Console viewport (centered on screen)
    uint32_t ViewportX;
    uint32_t ViewportY;
    uint32_t ViewportWidth;
    uint32_t ViewportHeight;
    // Console Font
    void* StandartFontBuffer;
    uint16_t FontSymbolSizeX;
    uint16_t FontSymbolSizeY;
    uint32_t FontNumberOfSymbols; // must be <= 256
    // MemoryMap
    void* MemoryMapAddress;
    uint32_t MemoryMapEntrySize;
    uint64_t MemoryMapEntriesNumber;
    uint64_t TotalMemorySize;
    uint64_t FreeMemorySize;
    // Other info about memory
    uint64_t KernelAddress;
    uint64_t KernelSize;
    uint64_t StartDataAddress;
    uint64_t StartDataSize;
} BOOT_HEADER;

typedef struct {
    uint64_t Start;
    uint64_t End;
    uint32_t Type;    // 0 - free memory, 
                    // 1 - my code and data, 
                    // 2 - EFI code and data, 
                    // 3 - ACPI data, 
                    // 4 - reserved and unusable (?), 
                    // 5 - mapped IO,
                    // 6 - other (?)
    uint32_t MemSize;
    uint64_t RESERVED1;
} MEMORY_MAP_ENTRY;

#endif