#include "stdio.h"
#include "string.h"
#include "../cpu/types.h"
#include "../drivers/screen.h"

void printf(char* str) {
    unsigned int i = 0;
    while(str[i] != '\0') {
        if (str[i] == '\n') {
            //Screen.CursorPositionX = 0;
            Screen.CursorPositionY += Screen.SymbolSizeY;
        } else if (str[i] == '\r') {
            Screen.CursorPositionX = 0;
        } else if (str[i] == '\t') {
            Screen.CursorPositionX += 4 * Screen.SymbolSizeX;
        } else {
            putChar(str[i]);
            Screen.CursorPositionX += Screen.SymbolSizeX;
        }
        i++;

        if (Screen.CursorPositionX + 8 > Screen.PixelsPerScanLine) {
            Screen.CursorPositionX = 0;
            Screen.CursorPositionY += Screen.SymbolSizeY;
        }
    }
}

void printi(int dec) {
    char temp_str[100];
    int_to_str(dec, temp_str);
    printf(temp_str);
}

void printh(UINT64 hex, UINT64 size) {
    char temp_str[size];
    hex_to_str(hex, temp_str, size);
    printf("0x");
    printf(temp_str);
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