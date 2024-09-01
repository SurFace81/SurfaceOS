#ifndef SCREEN_H
#define SCREEN_H

#include "../cpu/types.h"

typedef struct {
    UINT32 Width;
    UINT32 Height;
    UINT32 TextColor;

    void* BufferAddress;
    UINT64 BufferSize;
    UINT32 PixelsPerScanLine;

    UINT32 CursorPositionX;
    UINT32 CursorPositionY;

    void* FontPtr;
    UINT16 SymbolSizeX;
    UINT16 SymbolSizeY;
    UINT32 NumberOfSymbols;
} SYSTEM_SCREEN;

extern SYSTEM_SCREEN Screen;

#ifdef __cplusplus
extern "C" {
#endif

// Functions
void clear_screen();
void putChar(char chr);
void set_text_color(UINT32);
void set_cursor_position(UINT32 x, UINT32 y);

#ifdef __cplusplus
}
#endif

#endif