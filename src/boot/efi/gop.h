#ifndef GOP_H
#define GOP_H

#include "efi.h"

typedef struct {
    void* BaseAddress;
    UINT64 BufferSize;
    UINT32 Width;
    UINT32 Height;
    UINT32 PixelsPerScanLine;
    UINT32 PixelFormat;   // EFI_GRAPHICS_PIXEL_FORMAT of the selected mode
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
    framebuffer.PixelFormat = gop->Mode->Info->PixelFormat;

    // If the current mode has an ambiguous pixel format (BitMask/BltOnly),
    // try to find an equivalent-resolution mode with a known BGRX/RGBX
    // layout; otherwise keep what the firmware selected.
    if (framebuffer.PixelFormat > PixelBlueGreenRedReserved8BitPerColor)
    {
        for (UINT32 m = 0; m < gop->Mode->MaxMode; m++)
        {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;
            UINTN infoSize = 0;
            if (gop->QueryMode(gop, m, &infoSize, &info) != EFI_SUCCESS || !info)
                continue;
            if (info->HorizontalResolution == framebuffer.Width &&
                info->VerticalResolution == framebuffer.Height &&
                info->PixelFormat <= PixelBlueGreenRedReserved8BitPerColor)
            {
                if (gop->SetMode(gop, m) == EFI_SUCCESS)
                {
                    framebuffer.BaseAddress = (void*)gop->Mode->FrameBufferBase;
                    framebuffer.BufferSize = gop->Mode->FrameBufferSize;
                    framebuffer.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
                    framebuffer.PixelFormat = gop->Mode->Info->PixelFormat;
                }
                break;
            }
        }
    }

    return &framebuffer;
}

#endif