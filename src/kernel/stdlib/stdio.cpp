#include "../../include/stdlib/stdio.h"

void print(const char* str)
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
            screen::putChar((char)str[i]);
            Screen.CursorPosX += Screen.SymbolSizeX;
        }
        i++;
        if (Screen.CursorPosX + 8 > Screen.PixelsPerScanLine) {
            Screen.CursorPosX = 0;
            Screen.CursorPosY += Screen.SymbolSizeY;
        }
    }
}

void print(list::List<char>* char_list) {
    if (!char_list) return;
    
    list::Block<char>* current = char_list->first_block;
    
    while (current) {
        for (uint64_t i = 0; i < current->used; i++) {
            print(current->data[i]);
            if (current->data[i] == '\0') {
                return;
            }
        }
        current = current->next;
    }
}

void print(char c) {
    char out[2] = {c, '\0'};
    print(out);
}

void print(int dec)
{
    char temp_str[100];
    int_to_str(dec, temp_str);
    print(temp_str);
}

void print(uint64_t hex, uint64_t size) {
    char temp_str[size];
    hex_to_str(hex, temp_str, size);
    print("0x");
    print(temp_str);
}

void clearScreen() {
    screen::clear();
}

void setTextColor(Colors color) {
    screen::set_text_color(color);
}

void setCursorPosition(uint32_t x, uint32_t y) {
    screen::set_cursor_position(x, y);
}