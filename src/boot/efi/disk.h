#ifndef DISK_DEBUG_H
#define DISK_DEBUG_H

#include "efi.h"


EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;

void CloseFile(EFI_FILE_PROTOCOL* FileHandle) {
    FileHandle->Close(FileHandle);
}

void CopyFile(EFI_PHYSICAL_ADDRESS dest, void* buffer, UINT64 fileSize, EFI_SYSTEM_TABLE* SystemTable) {
    // Allocate pages
    SystemTable->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, fileSize / 4096 + 1, &dest);

    SystemTable->BootServices->CopyMem((void*)dest, buffer, fileSize);
}

EFI_FILE_PROTOCOL* OpenFile(CHAR16* fileName, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume) {
    EFI_FILE_PROTOCOL* RootFS;
    Volume->OpenVolume(Volume, &RootFS);

    EFI_FILE_PROTOCOL* FileHandle;
    RootFS->Open(RootFS, &FileHandle, fileName, EFI_FILE_MODE_READ, 0);

    return FileHandle;
}

EFI_FILE_INFO* GetFileInfo(EFI_FILE_PROTOCOL* FileHandle, EFI_SYSTEM_TABLE* SystemTable) {
    UINTN bufSize;
    EFI_FILE_INFO* buf;

    FileHandle->GetInfo(FileHandle, &EFI_FILE_INFO_GUID, &bufSize, NULL);

    SystemTable->BootServices->AllocatePool(EfiLoaderData, bufSize, (void**)&buf);

    FileHandle->GetInfo(FileHandle, &EFI_FILE_INFO_GUID, &bufSize, (void*)buf);

    return buf;
}

void LoadFile(CHAR16* fileName, EFI_SYSTEM_TABLE* SystemTable, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume, UINT64 Address, UINT64* FileSize) {
    EFI_FILE_PROTOCOL* file = OpenFile(fileName, Volume);
    void* buffer;

    if (file != NULL) {
        EFI_FILE_INFO* fileInfo = GetFileInfo(file, SystemTable);
        if (fileInfo != NULL) {
            UINT64 fileSize = fileInfo->FileSize;
            *FileSize = fileSize;

            SystemTable->BootServices->AllocatePool(EfiLoaderCode, fileSize, (void**)&buffer);

            file->Read(file, &fileSize, buffer);

            CopyFile(Address, buffer, fileSize, SystemTable);
        }

        CloseFile(file);
    }
}

#endif