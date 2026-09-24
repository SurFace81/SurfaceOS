#include "efi.h"
#include "error_codes.h"
#include "string.h"
#include "disk.h"
#include "gop.h"
#include "memory.h"
#include "bootheader.h"
#include "acpi.h"

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

    // ACPI root for the kernel; the loader itself needs it for DMAR below.
    ACPI_RSDP *Rsdp = acpi_find_rsdp(SystemTable);
    BootHeader.AcpiRsdpAddress = (UINT64)Rsdp;

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

    // Exit boot services. The MapKey from the memory map at the top is long
    // stale by now (every AllocatePool/AllocatePages/LoadFile since changed
    // the map), and ExitBootServices rejects a stale key. It used to be
    // called unchecked with that key, so it failed and the firmware - its
    // timers, its drivers, its DMA protection - stayed alive under the
    // kernel. Take a fresh key with no allocation in between; the spec
    // allows exactly one more GetMemoryMap + retry if that still races.
    // The map handed to the kernel stays the early one: nothing allocated
    // since then is memory the kernel may use anyway.
    //
    // The buffer is sized from the map as it is *now*, not from the early
    // one: GOP, the file system driver and every LoadFile since then split
    // the map into many more descriptors on real firmware, and a buffer
    // sized off the early map made GetMemoryMap fail with BUFFER_TOO_SMALL.
    // A failed ExitBootServices call may still run the firmware's
    // before-exit handlers, which allocate and change the map again, and
    // after a failed call only GetMemoryMap/ExitBootServices are allowed -
    // so the buffer gets generous slack up front and the loop retries a few
    // times rather than once.
    {
        EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
        EFI_MEMORY_DESCRIPTOR* ExitMap = NULL;
        UINTN ExitMapCapacity = 0;
        UINTN ExitMapSize = 0;
        EFI_STATUS ebs = EFI_BUFFER_TOO_SMALL;
        int attempt = 0;
        int ebsTried = 0;

        for (; attempt < 8; attempt++) {
            if (ebs == EFI_BUFFER_TOO_SMALL) {
                // Pool allocations are only legal before the first
                // ExitBootServices call.
                if (ebsTried)
                    break;
                if (ExitMap)
                    BS->FreePool(ExitMap);
                ExitMap = NULL;
                ExitMapSize = 0;
                BS->GetMemoryMap(&ExitMapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
                ExitMapCapacity = ExitMapSize + 64 * DescriptorSize;
                if (BS->AllocatePool(EfiLoaderData, ExitMapCapacity, (void**)&ExitMap) != EFI_SUCCESS) {
                    ebs = EFI_OUT_OF_RESOURCES;
                    break;
                }
            }

            ExitMapSize = ExitMapCapacity;
            ebs = BS->GetMemoryMap(&ExitMapSize, ExitMap, &MapKey, &DescriptorSize, &DescriptorVersion);
            if (ebs == EFI_BUFFER_TOO_SMALL)
                continue;
            if (ebs != EFI_SUCCESS)
                break;

            ebsTried = 1;
            ebs = BS->ExitBootServices(ImageHandle, MapKey);
            if (ebs != EFI_INVALID_PARAMETER)   // success, or not a stale key
                break;
        }

        if (ebs != EFI_SUCCESS) {
            // Status low byte, attempt, map size needed vs buffer (bytes):
            // enough to tell a stale key from a short buffer on hardware.
            CHAR16 msg[] = L"ExitBootServices failed! st=XX try=X map=XXXXX/XXXXX\n\rFatal error...";
            UINT64 fields[4] = { ebs & 0xFF, (UINT64)attempt, ExitMapSize, ExitMapCapacity };
            UINTN pos[4] = { 28, 35, 41, 47 }, width[4] = { 2, 1, 5, 5 };
            for (int f = 0; f < 4; f++)
                for (UINTN d = 0; d < width[f]; d++)
                    msg[pos[f] + d] = nums_table[(fields[f] >> (4 * (width[f] - 1 - d))) & 0xF];
            SystemTable->ConOut->OutputString(SystemTable->ConOut, msg);
            while(1){}
        }
    }

    // Boot services are gone, the firmware's page tables are still live:
    // switch the VT-d units off through their physical register addresses
    // and tell the kernel how it went.
    {
        SFOS_BOOT_HEADER *Header = (SFOS_BOOT_HEADER*)BootHeaderAddress;
        Header->DmarFlags = acpi_disable_dmar(Rsdp, &Header->DmarUnits, &Header->DmarDisabled);
    }

    Start((SFOS_BOOT_HEADER*)BootHeaderAddress);

    while(1) {}

    // We should not make it to this point.
    return EFI_SUCCESS;
}