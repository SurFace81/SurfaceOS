#ifndef SCREEN_H
#define SCREEN_H

#include "../../kernel/kernel.h"
#include "../cpu/types.h"

enum Colors
{
    BLUE = 0x000000FF,
    GREEN = 0x0000FF00,
    CYAN = 0x0000FFFF,
    RED = 0x00FF0000,
    MAGENTA = 0x00FF00FF,
    YELLOW = 0x00FFFF00,
    WHITE = 0x00FFFFFF,
    GRAY = 0x9E9E9EA8,
    LIGHT_BLUE = 0x0000AFFF,
    LIGHT_GREEN = 0x0000FFAA,
    LIGHT_AQUA = 0x00FFAAAA,
    LIGHT_RED = 0x00FF4444,
    LIGHT_PURPLE = 0x00FF55FF,
    LIGHT_YELLOW = 0x00FFFF88,
    BRIGHT_WHITE = 0x00F0F0F0,
};

namespace screen
{
    void init(BOOT_HEADER* header);

    // Dimensions
    uint32_t cols();
    uint32_t rows();
    uint32_t width();
    uint32_t height();

    // Cursor
    void set_cursor(uint32_t x, uint32_t y);
    uint32_t cursor_x();
    uint32_t cursor_y();

    // Output
    void putc(char c);
    void write(const char* s);
    void printf(const char* fmt, ...);

    // Screen operations
    void clear();
    void scroll_up();
    void erase_at(uint32_t x, uint32_t y);
    void set_color(Colors color);
} // namespace screen

#endif // SCREEN_H