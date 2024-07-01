#ifndef GOP_H
#define GOP_H

#include "efi.h"

typedef struct {
    void* BaseAddress;
    UINT64 BufferSize;
    UINT32 Width;
    UINT32 Height;
    UINT32 PixelsPerScanLine;
} Framebuffer;

Framebuffer framebuffer;
Framebuffer* initGOP(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;

    SystemTable->BootServices->LocateProtocol(&gopGuid, NULL, (void**)&gop);

    framebuffer.BaseAddress = (void*)gop->Mode->FrameBufferBase;
    framebuffer.BufferSize = gop->Mode->FrameBufferSize;
    framebuffer.Width = gop->Mode->Info->HorizontalResolution;
    framebuffer.Height = gop->Mode->Info->VerticalResolution;
    framebuffer.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;

    return &framebuffer;
}

#endif