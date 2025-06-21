#include "../../include/drivers/screen.h"

SYSTEM_SCREEN Screen;
const unsigned int BBP = 4;

namespace screen {
    void init(SYSTEM_SCREEN* Screen, SFOS_BOOT_HEADER* Header) {
        Screen->BufferAddress = (UINT8*)0x600000;
        Screen->BufferSize = Header->FrameBufferSize;
        Screen->CursorPosX = 0;
        Screen->CursorPosY = 0;
        Screen->FontPtr = (char*)Header->StandartFontBuffer;
        Screen->Height = Header->ScreenHeight;
        Screen->Width = Header->ScreenWidth;
        Screen->NumberOfSymbols = Header->FontNumberOfSymbols;
        Screen->PixelsPerScanLine = Header->ScreenPixelsPerScanLine;
        Screen->SymbolSizeX = Header->FontSymbolSizeX;
        Screen->SymbolSizeY = Header->FontSymbolSizeY;
        Screen->TextColor = 0x9E9E9EA8;     // - Gray color, (0x0000FFFF - Green; 0x00FF00FF - Red; 0xFF0000FF - Blue)

        clear();
    }

    void putChar(char chr) {
        unsigned int offsetX = Screen.CursorPosX;
        unsigned int offsetY = Screen.CursorPosY;

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

    void clear(void) {
        UINT8* bufferPtr = (UINT8*)Screen.BufferAddress;
        for (int i = 0; i < Screen.BufferSize; i++) {
            bufferPtr[i] = 0x00;
        }

        set_cursor_position(0, 0);
    }

    void set_text_color(UINT32 newColor) {
        Screen.TextColor = newColor;
    }

    void set_cursor_position(UINT32 x, UINT32 y) {
        Screen.CursorPosX = x;
        Screen.CursorPosY = y;
    }
} // namespace