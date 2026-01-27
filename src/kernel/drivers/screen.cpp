#include "../../include/drivers/screen.h"
#include "../../include/drivers/uart.h"

SYSTEM_SCREEN Screen;

namespace {
    static inline uint32_t cols() {
        return Screen.Width / Screen.SymbolSizeX;
    }

    static inline uint32_t rows() {
        return Screen.Height / Screen.SymbolSizeY;
    }
}

namespace screen {
    static const unsigned int BBP = 4;
    static uint8_t columnState[320] = {0}; // Maximum: 2560 pixels horizontal

    void init(SYSTEM_SCREEN* Screen, BOOT_HEADER* Header) {
        Screen->BufferAddress = (uint8_t*)0x600000;
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
        unsigned int offsetX = Screen.CursorPosX * Screen.SymbolSizeX;
        unsigned int offsetY = Screen.CursorPosY * Screen.SymbolSizeY;

        char* charPtr = Screen.FontPtr + chr * Screen.SymbolSizeY;

        for (unsigned int y = offsetY; y < offsetY + 16; y++) {
            for (unsigned int x = offsetX; x < offsetX + 8; x++) {
                if ((*charPtr & (0b10000000 >> (x - offsetX))) > 0) {
                    *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = Screen.TextColor;
                } else {
                    *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = 0x00000000;
                }
            }
            charPtr++;
        }
    }

    void clear(void) {
        uint32_t* bufferPtr = (uint32_t*)Screen.BufferAddress;
        for (unsigned int i = 0; i < Screen.BufferSize; i++) {
            bufferPtr[i] = 0x00000000;
        }

        set_cursor_position(0, 0);
    }

    static void eraseChar(unsigned int posX, unsigned int posY) {
        for (unsigned int y = posY; y < posY + Screen.SymbolSizeY; y++) {
            for (unsigned int x = posX; x < posX + Screen.SymbolSizeX; x++) {
                if (x < Screen.Width && y < Screen.Height) {
                    *(unsigned int*)(x * BBP + (y * Screen.PixelsPerScanLine * BBP) + Screen.BufferAddress) = 0x00000000;
                }
            }
        }
    }

    void backspace(uint32_t posX, uint32_t posY) {
        eraseChar(posX * Screen.SymbolSizeX, posY * Screen.SymbolSizeY);
    }

    void scroll_up(void) {
        const uint32_t lineHeight = Screen.SymbolSizeY;
        const uint32_t bytesPerLine = Screen.PixelsPerScanLine * BBP;
        
        uint64_t* src = (uint64_t*)Screen.BufferAddress + (lineHeight * bytesPerLine) / 8;
        uint64_t* dst = (uint64_t*)Screen.BufferAddress;
        
        const uint32_t copySize = ((rows() - 1) * lineHeight * bytesPerLine) / 8;
        
        for (uint32_t i = 0; i < copySize; i++) {
            dst[i] = src[i];
        }
        
        // Erase last line
        uint64_t* lastLine = (uint64_t*)Screen.BufferAddress;
        lastLine += ((rows() - 1) * lineHeight * bytesPerLine) / 8;
        
        const uint32_t lastLineSize = (lineHeight * bytesPerLine) / 8;
        for (uint32_t i = 0; i < lastLineSize; i++) {
            lastLine[i] = 0;
        }

        set_cursor_position(0, rows() - 1);
    }

    void set_text_color(Colors newColor) {
        Screen.TextColor = newColor;

        for (unsigned int y = 0; y < Screen.Height; y++) {
            for (unsigned int x = 0; x < Screen.Width; x++) {
                uint32_t* pixel = x + (y * Screen.PixelsPerScanLine) + (uint32_t*)Screen.BufferAddress;
                if (*pixel != 0x00000000) {
                    *pixel = (uint32_t)Screen.TextColor;
                }
            }
        }
    }

    void set_cursor_position(uint32_t x, uint32_t y) {
        Screen.CursorPosX = x;
        Screen.CursorPosY = y;
    }

    void putc(char c) {
        switch (c) {
            case '\n':
                for (uint32_t i = 0; i < cols(); i++) {
                    if (columnState[i] > 0) {
                        columnState[i] -= 1;
                    }
                }

                if (Screen.CursorPosY + 1 >= rows()) {
                    scroll_up();
                } else {
                    Screen.CursorPosY += 1;
                }
                break;

            case '\b':
                if (Screen.CursorPosX > 0) {
                    Screen.CursorPosX -= 1;
                }
                backspace(Screen.CursorPosX, Screen.CursorPosY);
                break;

            case '\t':
                Screen.CursorPosX += 4;
                break;

            case '\r':
                Screen.CursorPosX = 0;
                break;

            default:
                putChar(c);

                if (Screen.CursorPosX < cols()) {
                    columnState[Screen.CursorPosX] = rows();
                }

                Screen.CursorPosX += 1;
                if (Screen.CursorPosX >= cols()) {
                    Screen.CursorPosX = 0;
                    if (Screen.CursorPosY + 1 >= rows()) {
                        scroll_up();
                    } else {
                        Screen.CursorPosY += 1;
                    }
                }
                break;
        }
    }

    void write(const char* s) {
        while (*s) putc(*s++);
    }

    static void utoa(uint64_t v, char* b, uint32_t base) {
        char* p = b;
        do {
            uint8_t d = v % base;
            *p++ = d < 10 ? '0' + d : 'A' + d - 10;
            v /= base;
        } while (v);
        *p = 0;
        for (char *l = b, *r = p - 1; l < r; l++, r--) {
            char t = *l; *l = *r; *r = t;
        }
    }

    void printf(const char* fmt, ...) {
        __builtin_va_list a;
        __builtin_va_start(a, fmt);

        char buf[32];

        while (*fmt) {
            if (*fmt != '%') { putc(*fmt++); continue; }
            fmt++;

            bool ll = (*fmt == 'l' && fmt[1] == 'l');
            if (ll) fmt += 2;

            switch (*fmt) {
                case 's': write(__builtin_va_arg(a, char*)); break;
                case 'u':
                case 'i':
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 10);
                    write(buf); break;
                case 'x':
                    write("0x");
                    utoa(ll ? __builtin_va_arg(a, uint64_t)
                            : __builtin_va_arg(a, uint32_t), buf, 16);
                    write(buf); break;
                case '%': putc('%'); break;
            }
            fmt++;
        }
        __builtin_va_end(a);
    }
} // namespace