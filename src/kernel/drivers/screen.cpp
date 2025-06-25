#include "../../include/drivers/screen.h"

SYSTEM_SCREEN Screen;

namespace screen {
    static const unsigned int BBP = 4;
    static UINT32 BLACK_COLOR = 0x00000000;

    void init(SYSTEM_SCREEN* Screen, BOOT_HEADER* Header) {
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
        Screen->TextColor = Colors::GRAY;     // - Gray color, (0x0000FFFF - Green; 0x00FF00FF - Red; 0xFF0000FF - Blue)

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
                } else {
                    *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = BLACK_COLOR;
                }
            }
            charPtr++;
        }
    }

    void clear(void) {
        UINT32* bufferPtr = (UINT32*)Screen.BufferAddress;
        for (unsigned int i = 0; i < Screen.BufferSize; i++) {
            bufferPtr[i] = 0x00000000;
        }

        set_cursor_position(0, 0);
    }

    static void eraseChar(unsigned int posX, unsigned int posY) {
        for (unsigned int y = posY; y < posY + Screen.SymbolSizeY; y++) {
            for (unsigned int x = posX; x < posX + Screen.SymbolSizeX; x++) {
                if (x < Screen.Width && y < Screen.Height) {
                    *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = BLACK_COLOR;
                }
            }
        }
    }

    void backspace(UINT32 posX, UINT32 posY) {
        eraseChar(posX * Screen.SymbolSizeX, posY * Screen.SymbolSizeY);
    }

    void scroll_up(void) {
        unsigned int lineHeight = Screen.SymbolSizeY;
        unsigned int bytesPerLine = Screen.PixelsPerScanLine * BBP;
        
        UINT64* src = (UINT64*)Screen.BufferAddress + (lineHeight * bytesPerLine) / 8;
        UINT64* dst = (UINT64*)Screen.BufferAddress;
        
        unsigned int totalTextLines = Screen.Height / Screen.SymbolSizeY;
        unsigned int copySize = ((totalTextLines - 1) * lineHeight * bytesPerLine) / 8;
        
        for (unsigned int i = 0; i < copySize; i++) {
            dst[i] = src[i];
        }
        
        // Erase last line
        UINT64* lastLine = (UINT64*)Screen.BufferAddress;
        lastLine += ((totalTextLines - 1) * lineHeight * bytesPerLine) / 8;
        
        unsigned int lastLineSize = (lineHeight * bytesPerLine) / 8;
        for (unsigned int i = 0; i < lastLineSize; i++) {
            lastLine[i] = 0;
        }

        set_cursor_position(0, (totalTextLines - 1) * Screen.SymbolSizeY);
    }

    void set_text_color(Colors newColor) {
        Screen.TextColor = newColor;

        for (unsigned int y = 0; y < Screen.Height; y++) {
            for (unsigned int x = 0; x < Screen.Width; x++) {
                UINT32* pixel = x + (y * Screen.PixelsPerScanLine) + (UINT32*)Screen.BufferAddress;
                if (*pixel != 0x00000000) {
                    *pixel = (UINT32)Screen.TextColor;
                }
            }
        }
    }

    void set_cursor_position(UINT32 x, UINT32 y) {
        Screen.CursorPosX = x;
        Screen.CursorPosY = y;
    }
} // namespace