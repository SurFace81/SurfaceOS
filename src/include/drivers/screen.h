#ifndef SCREEN_H
#define SCREEN_H

#include "../cpu/types.h"
#include "../../kernel/kernel.h"

typedef struct {
    uint32_t Width;
    uint32_t Height;
    uint32_t TextColor;

    uint8_t* BufferAddress;
    uint64_t BufferSize;
    uint32_t PixelsPerScanLine;

    uint32_t CursorPosX;
    uint32_t CursorPosY;

    char* FontPtr;
    uint16_t SymbolSizeX;
    uint16_t SymbolSizeY;
    uint32_t NumberOfSymbols;
} SYSTEM_SCREEN;

enum Colors {
    BLUE         = 0x000000FF,
    GREEN        = 0x0000FF00,
    CYAN         = 0x0000FFFF,
    RED          = 0x00FF0000,
    MAGENTA      = 0x00FF00FF,
    YELLOW       = 0x00FFFF00,
    WHITE        = 0x00FFFFFF,
    GRAY         = 0x9E9E9EA8,
    LIGHT_BLUE   = 0x0000AFFF,
    LIGHT_GREEN  = 0x0000FFAA,
    LIGHT_AQUA   = 0x00FFAAAA,
    LIGHT_RED    = 0x00FF4444,
    LIGHT_PURPLE = 0x00FF55FF,
    LIGHT_YELLOW = 0x00FFFF88,
    BRIGHT_WHITE = 0x00F0F0F0,
};

extern SYSTEM_SCREEN Screen;

namespace screen {
    void init(SYSTEM_SCREEN* Screen, BOOT_HEADER* Header);
    void clear(void);
    void backspace(uint32_t posX, uint32_t posY);
    void scroll_up(void);
    void putChar(char chr);
    void set_text_color(Colors);
    void set_cursor_position(uint32_t x, uint32_t y);
    void printf(const char* fmt, ...);
}

#endif  // SCREEN_H