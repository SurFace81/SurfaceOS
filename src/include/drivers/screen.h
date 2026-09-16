#ifndef SCREEN_H
#define SCREEN_H

#include "../cpu/types.h"
#include "../boot/boot.h"

// The back buffer used to live at a fixed 0x600000. That is one full frame
// of pixels: ~15 MB on a 2K panel and ~33 MB at 4K, where it ran straight
// into the kernel heap at 0x2000000. It is now allocated from the PMM in
// screen::init(), so its size follows the panel instead of the other way
// round.

enum Colors
{
    BLUE = 0x000000FF,
    GREEN = 0x0000FF00,
    CYAN = 0x0000FFFF,
    RED = 0x00FF0000,
    MAGENTA = 0x00FF00FF,
    YELLOW = 0x00FFFF00,
    WHITE = 0x00FFFFFF,
    GRAY = 0x009E9E9E,
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

    // Cursor appearance
    void show_cursor();
    void hide_cursor();
    void update_cursor(); // call from timer to implement blinking

    // Output
    void putc(char c);
    void write(const char* s);
    void printf(const char* fmt, ...);

    // Screen operations
    void clear();
    void scroll_up();
    void erase_at(uint32_t x, uint32_t y);
    void set_color(Colors color);
    
    void flush();

    // Virtual address the framebuffer is mapped at (kernel device window).
    uint64_t vram_base();

    void push_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void pop_viewport();
    void draw_title_bar(const char* title);
    uint32_t title_bar_height();  // px, ~2.5% of screen height

    uint32_t vp_x();
    uint32_t vp_y();
    uint32_t vp_w();
    uint32_t vp_h();

} // namespace screen

#endif // SCREEN_H