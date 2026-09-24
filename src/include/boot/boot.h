#ifndef BOOT_H
#define BOOT_H

#include "../cpu/types.h"

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
    // GOP pixel format (EFI_GRAPHICS_PIXEL_FORMAT):
    // 0 = RGBX, 1 = BGRX, 2 = BitMask, 3 = BltOnly
    uint32_t ScreenPixelFormat;
    // Boot volume identification (stage 3.2), from the MEDIA_HARDDRIVE_DP
    // node of the booted image's device path. Must stay layout-identical
    // with SFOS_BOOT_HEADER in src/boot/efi/bootheader.h.
    uint64_t BootPartitionStart;         // LBA of the boot partition
    uint64_t BootPartitionSize;          // sectors
    uint32_t BootDevicePathValid;        // 1: a HARDDRIVE_DP node was found
    uint32_t BootPartitionSignatureType; // 0 none (superfloppy), 1 MBR, 2 GPT
    uint8_t  BootPartitionSignature[16]; // MBR: disk signature in [0..3]
                                         // GPT: disk GUID
    // ACPI: the RSDP (0: none found) and what the loader did to the VT-d
    // remapping units listed in DMAR before jumping here.
    uint64_t AcpiRsdpAddress;
    uint32_t DmarUnits;                  // DRHD units listed in DMAR
    uint32_t DmarDisabled;               // units that had remapping on, now off
    uint32_t DmarFlags;                  // BOOT_DMAR_* below
} BOOT_HEADER;

// BOOT_HEADER.DmarFlags, mirror DMAR_FLAG_* in src/boot/efi/acpi.h
#define BOOT_DMAR_PRESENT       (1U << 0)   // a DMAR table exists
#define BOOT_DMAR_WAS_ENABLED   (1U << 1)   // some unit had TE, IRE or EPM on
#define BOOT_DMAR_TIMEOUT       (1U << 2)   // some unit did not acknowledge

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