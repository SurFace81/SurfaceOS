// Terminal emulator: the cell grid, the cursor and (step 2) the escape
// parser. See term.h for why this is split from screen.cpp.

#include "../../include/drivers/term.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"

namespace
{
    term_cell* grid = nullptr;      // panel_cols * panel_rows, row stride = stride
    uint32_t   stride = 0;          // allocated columns per row
    uint32_t   grid_rows = 0;       // allocated rows

    uint32_t   cur_cols = 0;        // active area (follows the viewport)
    uint32_t   cur_rows = 0;

    uint32_t   cx = 0, cy = 0;      // cursor, in cells
    uint8_t    fg = TERM_DEFAULT_FG;
    uint8_t    bg = TERM_DEFAULT_BG;
    uint8_t    attr = 0;

    bool       cursor_on = false;   // visible at all
    bool       cursor_inverted = false;
    uint32_t   inv_x = 0, inv_y = 0;    // where the inversion currently sits

    // One dirty bit per row. 64 rows per word; a 4K panel is 135 rows.
    const uint32_t DIRTY_WORDS = 8;     // up to 512 rows
    uint64_t   dirty[DIRTY_WORDS];

    inline term_cell* cell_at(uint32_t x, uint32_t y)
    {
        return &grid[y * stride + x];
    }

    inline void mark_row(uint32_t y)
    {
        if (y < grid_rows)
            dirty[y >> 6] |= (1ULL << (y & 63));
    }

    inline bool row_dirty(uint32_t y)
    {
        return (dirty[y >> 6] >> (y & 63)) & 1;
    }

    inline void mark_all()
    {
        for (uint32_t i = 0; i < DIRTY_WORDS; i++)
            dirty[i] = ~0ULL;
    }

    void blank_row(uint32_t y)
    {
        if (y >= grid_rows)
            return;
        term_cell* r = cell_at(0, y);
        for (uint32_t x = 0; x < stride; x++)
        {
            r[x].ch = ' ';
            r[x].fg = fg;
            r[x].bg = bg;
            r[x].attr = 0;
        }
        mark_row(y);
    }

    // Advance one line, scrolling when the cursor falls off the bottom.
    void line_feed()
    {
        if (cy + 1 >= cur_rows)
            term::scroll_up();
        else
            cy++;
    }

    void put_glyph(char c)
    {
        if (cx >= cur_cols)     // only if something left the cursor stranded
        {
            cx = 0;
            line_feed();
        }

        term_cell* p = cell_at(cx, cy);
        p->ch = (uint8_t)c;
        p->fg = fg;
        p->bg = bg;
        p->attr = attr;
        mark_row(cy);

        // Wrap immediately rather than deferring it to the next glyph. Real
        // terminals defer, but the kernel console's line editor reads
        // cursor_x() back and assumes it is always < cols - with a deferred
        // wrap its backspace would step to the wrong row.
        cx++;
        if (cx >= cur_cols)
        {
            cx = 0;
            line_feed();
        }
    }
}

namespace term
{
    bool init(uint32_t panel_cols, uint32_t panel_rows)
    {
        if (panel_cols == 0 || panel_rows == 0)
            return false;
        if (panel_rows > DIRTY_WORDS * 64)
            panel_rows = DIRTY_WORDS * 64;

        uint64_t bytes = (uint64_t)panel_cols * panel_rows * sizeof(term_cell);
        grid = (term_cell*)kmalloc(bytes);
        if (!grid)
        {
            uart::printf("term: cannot allocate a %llu KB cell grid\n",
                         bytes / 1024);
            return false;
        }

        stride    = panel_cols;
        grid_rows = panel_rows;
        cur_cols  = panel_cols;
        cur_rows  = panel_rows;

        cx = cy = 0;
        fg = TERM_DEFAULT_FG;
        bg = TERM_DEFAULT_BG;
        attr = 0;
        cursor_on = false;
        cursor_inverted = false;

        clear();
        return true;
    }

    void resize(uint32_t c, uint32_t r)
    {
        if (!grid)
            return;
        if (c > stride)    c = stride;
        if (r > grid_rows) r = grid_rows;
        if (c == 0 || r == 0)
            return;

        cur_cols = c;
        cur_rows = r;
        if (cx >= cur_cols) cx = cur_cols ? cur_cols - 1 : 0;
        if (cy >= cur_rows) cy = cur_rows ? cur_rows - 1 : 0;
        cursor_inverted = false;    // the pixels under it are gone
        invalidate();
    }

    uint32_t cols() { return cur_cols; }
    uint32_t rows() { return cur_rows; }

    void putc(char c)
    {
        if (!grid)
            return;

        switch (c)
        {
            case '\n':
                cx = 0;
                line_feed();
                break;

            case '\r':
                cx = 0;
                break;

            case '\b':
                // Destructive backspace, matching what the old screen driver
                // did: step back and blank the cell landed on.
                if (cx > 0)
                    cx--;
                erase_at(cx, cy);
                break;

            case '\t':
                cx += 4;
                if (cx >= cur_cols)
                {
                    cx = 0;
                    line_feed();
                }
                break;

            default:
                put_glyph(c);
                break;
        }
    }

    void feed(const char* s, uint64_t len)
    {
        for (uint64_t i = 0; i < len; i++)
            putc(s[i]);
    }

    void clear()
    {
        if (!grid)
            return;
        for (uint32_t y = 0; y < grid_rows; y++)
            blank_row(y);
        cx = cy = 0;
        cursor_inverted = false;
        mark_all();
    }

    void scroll_up()
    {
        if (!grid || cur_rows == 0)
            return;

        // Move the rows up a line. The grid makes this a straight copy of
        // cur_rows-1 rows instead of the per-pixel loop this replaced.
        for (uint32_t y = 1; y < cur_rows; y++)
            memory::memcpy((uint8_t*)cell_at(0, y - 1),
                           (uint8_t*)cell_at(0, y),
                           cur_cols * sizeof(term_cell));

        // The vacated last row keeps the *current* colours, so a coloured
        // background scrolls without leaving a black band behind.
        term_cell* last = cell_at(0, cur_rows - 1);
        for (uint32_t x = 0; x < cur_cols; x++)
        {
            last[x].ch = ' ';
            last[x].fg = fg;
            last[x].bg = bg;
            last[x].attr = 0;
        }

        for (uint32_t y = 0; y < cur_rows; y++)
            mark_row(y);

        cx = 0;
        cy = cur_rows - 1;
        cursor_inverted = false;
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        if (!grid || x >= cur_cols || y >= cur_rows)
            return;
        term_cell* p = cell_at(x, y);
        p->ch = ' ';
        p->fg = fg;
        p->bg = bg;
        p->attr = 0;
        mark_row(y);
    }

    void set_cursor(uint32_t x, uint32_t y)
    {
        if (!grid)
            return;
        if (x >= cur_cols) x = cur_cols ? cur_cols - 1 : 0;
        if (y >= cur_rows) y = cur_rows ? cur_rows - 1 : 0;
        cx = x;
        cy = y;
    }

    uint32_t cursor_x() { return cx; }
    uint32_t cursor_y() { return cy; }

    void show_cursor() { cursor_on = true; }
    void hide_cursor() { cursor_on = false; }

    void set_fg(uint8_t idx) { fg = idx & 0x0F; }

    void invalidate()
    {
        mark_all();
    }

    void render()
    {
        if (!grid)
            return;

        // Lift the cursor first: it is an inversion of whatever was under
        // it, so it has to come off before anything repaints that cell.
        if (cursor_inverted)
        {
            screen::invert_cell(inv_x, inv_y);
            cursor_inverted = false;
        }

        for (uint32_t y = 0; y < cur_rows; y++)
        {
            if (!row_dirty(y))
                continue;
            term_cell* r = cell_at(0, y);
            for (uint32_t x = 0; x < cur_cols; x++)
                screen::draw_cell(x, y, r[x].ch, r[x].fg, r[x].bg, r[x].attr);
            dirty[y >> 6] &= ~(1ULL << (y & 63));
        }

        // Blink at 500 ms, same rate the old driver used.
        if (cursor_on && cx < cur_cols && cy < cur_rows)
        {
            uint32_t now = (uint32_t)pit::uptime_ms();
            if (((now / 500) % 2) == 0)
            {
                screen::invert_cell(cx, cy);
                cursor_inverted = true;
                inv_x = cx;
                inv_y = cy;
            }
        }
    }
}
