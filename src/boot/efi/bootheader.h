#ifndef BOOTHEADER_H
#define BOOTHEADER_H

#include "efi.h"

typedef struct {
    // Framebuffer
    void* FrameBufferAddress;
    UINT64 FrameBufferSize;
    UINT32 ScreenWidth;
    UINT32 ScreenHeight;
    UINT32 ScreenPixelsPerScanLine;
    // Console viewport (centered on screen)
    UINT32 ViewportX;
    UINT32 ViewportY;
    UINT32 ViewportWidth;
    UINT32 ViewportHeight;
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
    // GOP pixel format (EFI_GRAPHICS_PIXEL_FORMAT):
    // 0 = RGBX, 1 = BGRX, 2 = BitMask, 3 = BltOnly
    UINT32 ScreenPixelFormat;
    // Boot volume identification (stage 3.2). Taken from the
    // MEDIA_HARDDRIVE_DP node of LoadedImage->DeviceHandle's device path:
    // where on which disk the booted volume lives and how that disk is
    // signed, so the kernel can find the same volume through its own USB
    // driver and mount it as /. All zeros when no hard-drive node exists.
    UINT64 BootPartitionStart;         // LBA of the boot partition
    UINT64 BootPartitionSize;          // sectors
    UINT32 BootDevicePathValid;        // 1: a HARDDRIVE_DP node was found
    UINT32 BootPartitionSignatureType; // 0 none (superfloppy), 1 MBR, 2 GPT
    UINT8  BootPartitionSignature[16]; // MBR: disk signature in [0..3]
                                       // GPT: disk GUID
} SFOS_BOOT_HEADER;

#pragma pack(push, 1)
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
    UINT64 MemSize;
    UINT32 RESERVED1;
} MEMORY_MAP_ENTRY;
#pragma pack(pop)


#endif