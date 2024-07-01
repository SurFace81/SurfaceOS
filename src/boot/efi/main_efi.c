#include "efi.h"
#include "error_codes.h"
#include "string.h"
#include "disk.h"
#include "gop.h"
#include "memory.h"
#include "bootheader.h"

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    // Disable WatchdogTimer
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    // BootHeader
    SURFOS_BOOT_HEADER BootHeader;

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

        SystemTable->BootServices->HandleProtocol(LoadedImage->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&Volume);
    }

    // work with GOP
    Framebuffer* newBuffer = initGOP(SystemTable);

    // Set FB entry in BootHeader
    BootHeader.FrameBufferAddress = newBuffer->BaseAddress;
    BootHeader.FrameBufferSize = newBuffer->BufferSize;
    BootHeader.ScreenWidth = newBuffer->Width;
    BootHeader.ScreenHeight = newBuffer->Height;
    BootHeader.ScreenPixelsPerScanLine = newBuffer->PixelsPerScanLine;

    // Load STDFont and set STDFont entry in BootHeader
    UINT64 FontAddress = 0x102000;
    LoadFile(u"font.fnt", SystemTable, Volume, FontAddress, NULL);
    BootHeader.StandartFontBuffer = (void*)FontAddress;
    BootHeader.FontSymbolSizeX = 8;
    BootHeader.FontSymbolSizeY = 16;
    BootHeader.FontNumberOfSymbols = 256;

    // Set space for paging
    EFI_PHYSICAL_ADDRESS PagingSpace = (EFI_PHYSICAL_ADDRESS)0x300000;
    SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, (5 * 1024 * 1024) / 4096, &PagingSpace); // 5 MB for 2 GB memory

    // Copy BootHeader
    EFI_PHYSICAL_ADDRESS BootHeaderAddress = (EFI_PHYSICAL_ADDRESS)0x100000;
    //SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, 4, &BootHeaderAddress);
    SystemTable->BootServices->CopyMem((void*)BootHeaderAddress, (void*)&BootHeader, sizeof(BootHeader));

    // Load a kernel
    BootHeader.KernelAddress = 0x200000;  // <---Bug! this is not copied! Must be before `Copy Bootloader`.
    LoadFile(u"kernel.bin", SystemTable, Volume, BootHeader.KernelAddress, &BootHeader.KernelSize);

    // Reset screen and disbale cursor again
    {
        SystemTable->ConOut->Reset(SystemTable->ConOut, 1);
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        SystemTable->ConOut->EnableCursor(SystemTable->ConOut, 0);
    }

    // Call kernel
    void (__attribute__((sysv_abi)) *Start)(SURFOS_BOOT_HEADER*) = ((__attribute__((sysv_abi)) void (*)(SURFOS_BOOT_HEADER*) ) BootHeader.KernelAddress);

    SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);
    Start((SURFOS_BOOT_HEADER*)BootHeaderAddress);

    while(1) {}

    // We should not make it to this point.
    return EFI_SUCCESS;
}