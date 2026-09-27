#ifndef TERM_H
#define TERM_H

#include "../cpu/types.h"

// Terminal emulator (stage 4).
//
// `term` owns the logical state of the console: a grid of character cells
// with their attributes, the cursor, the scroll region and - from step 2 -
// the escape-sequence parser. `screen` below it owns pixels only: it knows
// how to rasterise one cell and how to get the back buffer onto the panel.
//
// The split exists because the console has to *remember* what is on it.
// Applications address individual cells and never repaint the whole screen,
// so scrolling, erasing and the alternate screen all need the previous
// contents. Pixels alone cannot answer "what character is at (12, 3)?".
//
// Writes only touch the grid and mark the row dirty; nothing is rasterised
// until render(), which the timer calls just before flushing to VRAM. A
// write interrupted by that timer is harmless: the cell is marked dirty
// after it is stored, so a half-written row is picked up on the next pass.

// Cell attributes
#define TERM_BOLD       0x01
#define TERM_REVERSE    0x02

// ANSI colour indices (0-7 normal, 8-15 bright).
#define TERM_BLACK      0
#define TERM_RED        1
#define TERM_GREEN      2
#define TERM_YELLOW     3
#define TERM_BLUE       4
#define TERM_MAGENTA    5
#define TERM_CYAN       6
#define TERM_WHITE      7
#define TERM_DEFAULT_FG TERM_WHITE
#define TERM_DEFAULT_BG TERM_BLACK

struct term_cell
{
    uint8_t ch;
    uint8_t fg;         // ANSI index 0..15
    uint8_t bg;
    uint8_t attr;       // TERM_*
};

// Screens: each has its own cells, cursor, colours and parser state, and
// the panel shows one of them (Alt+F1..F9, keyboard.cpp). Past those nine,
// hidden screens for programs running in the background: never shown,
// their grids taken only while one is in use.
#define TERM_SCREENS        9
#define TERM_HIDDEN_SCREENS 16
#define TERM_ALL_SCREENS    (TERM_SCREENS + TERM_HIDDEN_SCREENS)

namespace term
{
    // Allocates every screen's grid for the whole panel once; resize() then
    // selects a sub-rectangle of it, so a viewport change never reallocates.
    bool init(uint32_t panel_cols, uint32_t panel_rows);
    void resize(uint32_t cols, uint32_t rows);

    // Everything below writes to the selected screen (any in use); the
    // panel shows one of the first TERM_SCREENS, another or the same.
    void select(uint32_t n);
    uint32_t selected();
    void show(uint32_t n);
    uint32_t shown_screen();

    // A hidden screen for a background program: its number, or -1 when
    // none is free. close_hidden gives it back.
    sint32_t open_hidden();
    void     close_hidden(uint32_t n);

    // The selected screen's part of the title bar, which the system draws:
    // "F<n> | program | subtitle ... hh:mm". A new program clears the
    // subtitle.
    void set_program(const char* name);
    void set_subtitle(const char* text);
    // Screen n's programs are paused (Ctrl+Alt+Z): "| paused" at the end.
    void set_paused(uint32_t n, bool paused);

    uint32_t cols();
    uint32_t rows();

    // Bytes in, cells out. Control characters are handled here; escape
    // sequences join them in step 2.
    void feed(const char* s, uint64_t len);
    void putc(char c);

    // Screen operations, in cell coordinates.
    void clear();
    void scroll_up();
    void erase_at(uint32_t x, uint32_t y);

    // Cursor
    void set_cursor(uint32_t x, uint32_t y);
    uint32_t cursor_x();
    uint32_t cursor_y();
    void show_cursor();
    void hide_cursor();

    // Default foreground (and background) for cells written from now on.
    void set_fg(uint8_t idx);
    void set_colors(uint8_t fg, uint8_t bg);

    // Cells at a place, the cursor left alone: one in the given colours,
    // or a run of text in the current ones, cut at the right edge.
    void put_cell(uint32_t x, uint32_t y, char ch, uint8_t fg, uint8_t bg);
    void write_at(uint32_t x, uint32_t y, const char* s, uint64_t len);

    // Rasterise everything dirty (title bar included) into the back buffer
    // and update the cursor's blink state. Called from the timer before
    // screen::flush().
    void render();
    // Mark the whole grid dirty (after a viewport change or a repaint).
    void invalidate();
}

#endif // TERM_H
