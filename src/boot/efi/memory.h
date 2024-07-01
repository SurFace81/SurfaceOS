#ifndef MEMORY_H
#define MEMORY_H

#include "efi.h"

// After ExitBootServices():
// We may use this types of memory:
//      EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData, EfiConventionalMemory
// Must preserve:
//      EfiRuntimeServiceCode, EfiRuntimeServicesData
CHAR16* EFI_MEMORY_TYPE_STRINGS[16] =
{
    L"EfiReservedMemoryType",   // 0
    L"EfiLoaderCode",           // 1
    L"EfiLoaderData",           // 2
    L"EfiBootServicesCode",     // 3
    L"EfiBootServicesData",     // 4
    L"EfiRuntimeServicesCode",  // 5
    L"EfiRuntimeServicesData",  // 6
    L"EfiConventionalMemory",   // 7
    L"EfiUnusableMemory",       // 8
    L"EfiACPIReclaimMemory",    // 9
    L"EfiACPIMemoryNVS",        // 10
    L"EfiMemoryMappedIO",       // 11
    L"EfiMemoryMappedIOPortSpace", // 12
    L"EfiPalCode",              // 13
    L"EfiPersistentMemory",     // 14
    L"EfiMaxMemoryType"         // 15
};

UINT32 getEntryType(UINT32 type) {
    if (type == 1 || type == 2 || type == 3 || type == 4 || type == 7) {
        return 0;   // free memory
    } else if (type == 5 || type == 6) {
        return 2;   // EFI code / data
    } else if (type == 9 || type == 10) {
        return 3;   // ACPI data
    } else if (type == 0 || type == 8) {
        return 4;   // reserved / unusable
    } else if (type == 11 || type == 12) {
        return 5;   // mapped IO
    } else {
        return 6;   // other (can i use it?); (Types - 13, 14, 15)
    }
}

#endif