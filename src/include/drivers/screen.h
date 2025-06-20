#ifndef SCREEN_H
#define SCREEN_H

#include "../cpu/types.h"
#include "../../kernel/kernel.h"

typedef struct {
    UINT32 Width;
    UINT32 Height;
    UINT32 TextColor;

    UINT8* BufferAddress;
    UINT64 BufferSize;
    UINT32 PixelsPerScanLine;

    UINT32 CursorPosX;
    UINT32 CursorPosY;

    char* FontPtr;
    UINT16 SymbolSizeX;
    UINT16 SymbolSizeY;
    UINT32 NumberOfSymbols;
} SYSTEM_SCREEN;

extern SYSTEM_SCREEN Screen;

namespace screen {
    void init(SYSTEM_SCREEN* Screen, SFOS_BOOT_HEADER* Header);
    void clear();
    void putChar(char chr);
    void set_text_color(UINT32);
    void set_cursor_position(UINT32 x, UINT32 y);
}

#endif  // SCREEN_H