#include "efi.h"
#include "error_codes.h"
#include "string.h"
#include "disk.h"
#include "gop.h"
#include "memory.h"
#include "bootheader.h"

// MEDIA_HARDDRIVE_DP node (UEFI spec, Media Device Path, subtype 1).
// Laid over EFI_DEVICE_PATH_PROTOCOL by hand: efi.h does not define it.
// packed: the on-wire node is exactly 42 bytes; natural alignment would pad
// UINT64 PartitionStart and inflate sizeof() to 48, so the length check
// `len >= sizeof(...)` would skip every real node.
typedef struct {
    UINT8   Type;           // 4  (MEDIA_DEVICE_PATH)
    UINT8   SubType;        // 1  (MEDIA_HARDDRIVE_DP)
    UINT8   Length[2];
    UINT32  PartitionNumber;
    UINT64  PartitionStart; // LBA
    UINT64  PartitionSize;  // in LBAs
    UINT8   Signature[16];  // MBR: disk signature (first 4 bytes used)
                            // GPT: disk GUID
    UINT8   MBRType;        // 1: MBR w/ 0x55AA, 2: GPT protective MBR
    UINT8   SignatureType;  // 0: none, 1: MBR, 2: GUID
} __attribute__((packed)) HARDDRIVE_DEVICE_PATH;

#define MEDIA_DEVICE_PATH   4
#define MEDIA_HARDDRIVE_DP  1
#define END_DEVICE_PATH     0x7F

// Walk a device path and copy the first hard-drive node's partition info
// into the boot header. Superfloppies (whole-disk media nodes) have no
// HARDDRIVE_DP: everything stays zero, and the kernel mounts the volume at
// LBA 0.
static void fill_boot_partition_info(EFI_DEVICE_PATH_PROTOCOL *DevicePath,
                                     SFOS_BOOT_HEADER *BootHeader)
{
    BootHeader->BootPartitionStart = 0;
    BootHeader->BootPartitionSize = 0;
    BootHeader->BootDevicePathValid = 0;
    BootHeader->BootPartitionSignatureType = 0;
    for (int i = 0; i < 16; i++)
        BootHeader->BootPartitionSignature[i] = 0;

    if (!DevicePath)
        return;

    // Debug aid: record the (Type,SubType) pairs of up to 8 walked nodes in
    // the signature bytes when no HARDDRIVE_DP is found; the kernel logs it.
    UINT8 trace[16];
    for (int i = 0; i < 16; i++)
        trace[i] = 0;
    int ti = 0;

    for (;;) {
        UINT16 len = (UINT16)(DevicePath->Length[0] | (DevicePath->Length[1] << 8));
        if (len < 4)
            break;

        if (ti < 16)
            trace[ti++] = DevicePath->Type;
        if (ti < 16)
            trace[ti++] = DevicePath->SubType;

        if (DevicePath->Type == MEDIA_DEVICE_PATH &&
            DevicePath->SubType == MEDIA_HARDDRIVE_DP &&
            len >= sizeof(HARDDRIVE_DEVICE_PATH))
        {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)DevicePath;
            BootHeader->BootPartitionStart = hd->PartitionStart;
            BootHeader->BootPartitionSize = hd->PartitionSize;
            BootHeader->BootDevicePathValid = 1;
            BootHeader->BootPartitionSignatureType = hd->SignatureType;
            for (int i = 0; i < 16; i++)
                BootHeader->BootPartitionSignature[i] = hd->Signature[i];
            return;
        }

        if (DevicePath->Type == END_DEVICE_PATH)
            break;

        DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)DevicePath + len);
    }

    for (int i = 0; i < 16; i++)
        BootHeader->BootPartitionSignature[i] = trace[i];
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    // Disable WatchdogTimer
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    // BootHeader. Zeroed: the kernel reads fields the loader never writes
    // (boot partition info on a superfloppy, and anything added later), and
    // garbage there silently misidentified the root volume.
    SFOS_BOOT_HEADER BootHeader;
    for (UINTN i = 0; i < sizeof(BootHeader); i++)
        ((UINT8 *)&BootHeader)[i] = 0;

    // Reset screen and disbale cursor
    {
        SystemTable->ConOut->Reset(SystemTable->ConOut, 1);
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        SystemTable->ConOut->EnableCursor(SystemTable->ConOut, 0);
    }

    // Allocate memory for StartData (BootHeader, Font, MemoryMap)
    BootHeader.StartDataAddress = 0x100000;
    BootHeader.StartDataSize = 0x8000;
    EFI_PHYSICAL_ADDRESS StartData = (EFI_PHYSICAL_ADDRESS)BootHeader.StartDataAddress;
    EFI_STATUS s = SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, BootHeader.StartDataSize / 0x1000, &StartData);
    if (s != EFI_SUCCESS) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"StartData memory allocate error!\n\rFatal error...");
        while(1){}
    }

    // Get memory map
    UINTN                   MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR*  MemoryMap;
    UINTN                   MapKey;
    UINTN                   DescriptorSize;
    UINT32                  DescriptorVersion;
 
    {
        SystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

        MemoryMapSize += 2 * DescriptorSize;
    
        SystemTable->BootServices->AllocatePool(EfiLoaderData, MemoryMapSize, (void**)&MemoryMap);
        SystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    }

    // Parse memory map
    BootHeader.MemoryMapAddress = (void*)0x104000;
    if (MemoryMapSize > 0x4000) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Memory Map can't be placed!\n\rFatal error...");
        while(1){}
    }

    UINT64 totalMemory = 0, freeMemory = 0;
    void* lastEntry = BootHeader.MemoryMapAddress;
    MEMORY_MAP_ENTRY Entry;
    BootHeader.MemoryMapEntrySize = sizeof(Entry);

    {
        EFI_MEMORY_DESCRIPTOR* desc = MemoryMap;
        Entry.Start = desc->PhysicalStart;
        Entry.End = Entry.Start + desc->NumberOfPages * 4096;
        Entry.RESERVED1 = 0;

        Entry.Type = getEntryType(desc->Type);
    }    

    UINT64 entNum = 0;
    for (long long i = 1; i < MemoryMapSize / DescriptorSize; i++) {
        EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)((UINT64)MemoryMap + (i * DescriptorSize));

        if (getEntryType(desc->Type) == Entry.Type) {
            Entry.End += desc->NumberOfPages * 4096;
        } else {
            entNum++;

            Entry.MemSize = Entry.End - Entry.Start;
            if (Entry.Type == 0 || Entry.Type == 1) {
                freeMemory += Entry.MemSize;
            }

            SystemTable->BootServices->CopyMem(lastEntry, (void*)&Entry, BootHeader.MemoryMapEntrySize);
            lastEntry += BootHeader.MemoryMapEntrySize;

            Entry.Start = desc->PhysicalStart;
            Entry.End = Entry.Start + desc->NumberOfPages * 4096;
            Entry.Type = getEntryType(desc->Type);
        }
        
        totalMemory += desc->NumberOfPages * 4096;
    }
    BootHeader.TotalMemorySize = totalMemory;
    BootHeader.FreeMemorySize = freeMemory;
    BootHeader.MemoryMapEntriesNumber = entNum;


    // Prepare to work with a file
    {
        EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
        SystemTable->BootServices->HandleProtocol(ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void**)&LoadedImage);

        EFI_DEVICE_PATH_PROTOCOL *DevicePath;
        SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void**)&DevicePath);

        // Identify the boot volume for the kernel's automount (stage 3.2).
        fill_boot_partition_info(DevicePath, &BootHeader);

        SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&Volume);
    }

    // work with GOP
    Framebuffer* newBuffer = initGOP(SystemTable);

    // Comment out to use full screen
    // #define CONSOLE_WIDTH  1920
    // #define CONSOLE_HEIGHT 1080

    #ifdef CONSOLE_WIDTH
        UINT32 vw = (newBuffer->Width  >= CONSOLE_WIDTH)  ? CONSOLE_WIDTH  : newBuffer->Width;
        UINT32 vh = (newBuffer->Height >= CONSOLE_HEIGHT) ? CONSOLE_HEIGHT : newBuffer->Height;
    #else
        UINT32 vw = newBuffer->Width;
        UINT32 vh = newBuffer->Height;
    #endif

    BootHeader.ViewportX      = (newBuffer->Width  - vw) / 2;
    BootHeader.ViewportY      = (newBuffer->Height - vh) / 2;
    BootHeader.ViewportWidth  = vw;
    BootHeader.ViewportHeight = vh;

    // Set FB entry in BootHeader
    BootHeader.FrameBufferAddress = newBuffer->BaseAddress;
    BootHeader.FrameBufferSize = newBuffer->BufferSize;
    BootHeader.ScreenWidth = newBuffer->Width;
    BootHeader.ScreenHeight = newBuffer->Height;
    BootHeader.ScreenPixelsPerScanLine = newBuffer->PixelsPerScanLine;
    BootHeader.ScreenPixelFormat = newBuffer->PixelFormat;

    // Load STDFont and set STDFont entry in BootHeader
    UINT64 FontAddress = 0x4000;
    LoadFile(u"font.fnt", SystemTable, Volume, FontAddress, NULL);
    BootHeader.StandartFontBuffer = (void*)FontAddress;
    BootHeader.FontSymbolSizeX = 8;
    BootHeader.FontSymbolSizeY = 16;
    BootHeader.FontNumberOfSymbols = 256;

    // Set space for paging
    EFI_PHYSICAL_ADDRESS PagingSpace = (EFI_PHYSICAL_ADDRESS)0x300000;
    SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, (5 * 1024 * 1024) / 4096, &PagingSpace); // 5 MB for 2 GB memory

    // Kernel load address. Must be set before the BootHeader copy: it used
    // to be assigned after, so the kernel saw garbage in KernelAddress.
    BootHeader.KernelAddress = 0x200000;

    // Copy BootHeader
    EFI_PHYSICAL_ADDRESS BootHeaderAddress = (EFI_PHYSICAL_ADDRESS)0x100000;
    SystemTable->BootServices->CopyMem((void*)BootHeaderAddress, (void*)&BootHeader, sizeof(BootHeader));

    // Load a kernel. The pages are claimed from the firmware first: loading
    // into an address the firmware still considers free means anything it
    // decides to allocate afterwards can land on top of the kernel image.
    // 1 MB covers the image plus its .bss, and linker.ld asserts the kernel
    // stops before the page tables at 0x300000.
    EFI_PHYSICAL_ADDRESS KernelSpace = (EFI_PHYSICAL_ADDRESS)BootHeader.KernelAddress;
    if (SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData,
                                                 0x100000 / 0x1000, &KernelSpace) != EFI_SUCCESS) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Kernel memory allocate error!\n\rFatal error...");
        while(1){}
    }
    LoadFile(u"kernel.bin", SystemTable, Volume, BootHeader.KernelAddress, &BootHeader.KernelSize);
    // KernelSize is discovered after the copy: write it into the header that
    // the kernel will actually read.
    ((SFOS_BOOT_HEADER*)BootHeaderAddress)->KernelSize = BootHeader.KernelSize;

    // Reset screen and disbale cursor again
    {
        SystemTable->ConOut->Reset(SystemTable->ConOut, 1);
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        SystemTable->ConOut->EnableCursor(SystemTable->ConOut, 0);
    }

    // Call kernel
    void (__attribute__((sysv_abi)) *Start)(SFOS_BOOT_HEADER*) = ((__attribute__((sysv_abi)) void (*)(SFOS_BOOT_HEADER*) ) BootHeader.KernelAddress);

    SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);
    Start((SFOS_BOOT_HEADER*)BootHeaderAddress);

    while(1) {}

    // We should not make it to this point.
    return EFI_SUCCESS;
}