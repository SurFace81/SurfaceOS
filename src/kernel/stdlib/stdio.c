#include "../../include/stdlib/stdio.h"

void print_str(const char* str)
{
    unsigned int i = 0;
    while(str[i] != '\0') {
        if (str[i] == '\n') {
            Screen.CursorPosY += Screen.SymbolSizeY;
        } else if (str[i] == '\r') {
            Screen.CursorPosX = 0;
        } else if (str[i] == '\t') {
            Screen.CursorPosX += 4 * Screen.SymbolSizeX;
        } else {
            putChar((char)str[i]);
            Screen.CursorPosX += Screen.SymbolSizeX;
        }
        i++;

        if (Screen.CursorPosX + 8 > Screen.PixelsPerScanLine) {
            Screen.CursorPosX = 0;
            Screen.CursorPosY += Screen.SymbolSizeY;
        }
    }
}

void print_dec(int dec)
{
    char temp_str[100];
    int_to_str(dec, temp_str);
    print_str(temp_str);
}

void print_hex(UINT64 hex, UINT64 size) {
    char temp_str[size];
    hex_to_str(hex, temp_str, size);
    print_str("0x");
    print_str(temp_str);
}

void clearScreen() {
    clear_screen();
}

void setTextColor(UINT32 color) {
    set_text_color(color);
}

void setCursorPosition(UINT32 x, UINT32 y) {
    set_cursor_position(x, y);
}