#include "screen.h"

SYSTEM_SCREEN Screen;

void putChar(char chr) {
    unsigned int offsetX = Screen.CursorPositionX;
    unsigned int offsetY = Screen.CursorPositionY;

    const unsigned int BBP = 4;

    char* charPtr = Screen.FontPtr + chr * Screen.SymbolSizeY;

    for (unsigned int y = offsetY; y < offsetY + 16; y++) {
        for (unsigned int x = offsetX; x < offsetX + 8; x++) {
            if ((*charPtr & (0b10000000 >> (x - offsetX))) > 0) {
                *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = Screen.TextColor;
            }
        }
        charPtr++;
    }
}

void clear_screen() {
    UINT64* bufferPtr = (UINT64*)Screen.BufferAddress;
    for (int i = 0; i < Screen.BufferSize; i++) {
        bufferPtr[i] = 0x00;
    }

    Screen.CursorPositionX = 0;
    Screen.CursorPositionY = 0;
}

void set_text_color(UINT32 newColor) {
    Screen.TextColor = newColor;
}

void set_cursor_position(UINT32 x, UINT32 y) {
    Screen.CursorPositionX = x;
    Screen.CursorPositionY = y;
}