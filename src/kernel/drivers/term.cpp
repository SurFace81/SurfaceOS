// The screens' text: the cell grids, the cursors and the title bar. See
// term.h for why this is split from screen.cpp.

#include "../../include/drivers/term.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/rtc.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"

namespace
{
    // Panel geometry, the same for every screen.
    uint32_t   stride = 0;          // allocated columns per row
    uint32_t   grid_rows = 0;       // allocated rows

    uint32_t   cur_cols = 0;        // active area (follows the viewport)
    uint32_t   cur_rows = 0;

    // The blinking cursor on the panel (the shown screen's).
    bool       cursor_inverted = false;
    uint32_t   inv_x = 0, inv_y = 0;    // where the inversion currently sits

    // One dirty bit per row of the shown screen. 64 rows per word; a 4K
    // panel is 135 rows.
    const uint32_t DIRTY_WORDS = 8;     // up to 512 rows
    uint64_t   dirty[DIRTY_WORDS];

    // One screen: its cells, cursor and colours. Output goes to the
    // selected one (S); only the shown one is drawn.
    struct Screen
    {
        term_cell* grid;            // stride columns per row
        uint32_t   cx, cy;          // cursor, in cells
        uint8_t    fg, bg;
        bool       cursor_on;       // visible at all

        // --- title bar ----------------------------------------------------
        char       program[32];     // who runs on it; "" for none
        char       subtitle[64];    // the program's own words
        bool       paused;          // its programs are paused (Ctrl+Alt+Z)
    };

    Screen  screens[TERM_ALL_SCREENS];
    Screen* S     = &screens[0];    // output goes here
    Screen* shown = &screens[0];    // on the panel

    // The title bar is drawn again when the shown screen's text or the
    // minute on the clock changes.
    bool     title_dirty = true;
    uint32_t clock_minutes = ~0U;   // hours * 60 + minutes, as last drawn
    uint64_t clock_second = ~0ULL;  // uptime second the RTC was last read

    // The system's own words, on every screen's title bar before the
    // clock (set_status); they go at status_until (ms of uptime, 0: never).
    char     status[64];
    uint64_t status_until = 0;

    void copy_text(char* dst, uint32_t size, const char* src)
    {
        uint32_t i = 0;
        for (; src && src[i] && i + 1 < size; i++)
            dst[i] = src[i];
        dst[i] = 0;
    }

    char* append(char* p, char* end, const char* s)
    {
        while (*s && p < end)
            *p++ = *s++;
        return p;
    }

    // "F1 | console | subtitle" on the left, "15:30" on the right.
    void draw_title()
    {
        // The clock: the RTC is read once a second at most.
        uint64_t second = pit::uptime_ms() / 1000;
        if (second != clock_second)
        {
            clock_second = second;
            rtc_time t;
            rtc::read(&t);
            uint32_t m = t.hours * 60U + t.minutes;
            if (m != clock_minutes)
            {
                clock_minutes = m;
                title_dirty = true;
            }
        }
        if (status[0] && status_until && pit::uptime_ms() >= status_until)
        {
            status[0] = 0;
            title_dirty = true;
        }
        if (!title_dirty)
            return;
        title_dirty = false;

        char left[128];
        char* p = left;
        char* end = left + sizeof(left) - 1;
        char num[3] = { 'F', (char)('1' + (shown - screens)), 0 };
        p = append(p, end, num);
        if (shown->program[0])
        {
            p = append(p, end, " | ");
            p = append(p, end, shown->program);
        }
        if (shown->subtitle[0])
        {
            p = append(p, end, " | ");
            p = append(p, end, shown->subtitle);
        }
        if (shown->paused)
            p = append(p, end, " | paused");
        *p = 0;

        uint32_t h = clock_minutes / 60, m = clock_minutes % 60;
        char clock[6] = { (char)('0' + h / 10), (char)('0' + h % 10), ':',
                          (char)('0' + m / 10), (char)('0' + m % 10), 0 };
        char right[sizeof(status) + 16];
        char* r = right;
        char* rend = right + sizeof(right) - 1;
        if (status[0])
        {
            r = append(r, rend, status);
            r = append(r, rend, "  ");
        }
        r = append(r, rend, clock);
        *r = 0;
        screen::draw_title_bar(left, right);
    }

    inline term_cell* cell_at(uint32_t x, uint32_t y)
    {
        return &S->grid[y * stride + x];
    }

    // Writes to a screen that is not shown only change its cells.
    inline void mark_row(uint32_t y)
    {
        if (S == shown && y < grid_rows)
            dirty[y >> 6] |= (1ULL << (y & 63));
    }

    inline bool row_dirty(uint32_t y)
    {
        return (dirty[y >> 6] >> (y & 63)) & 1;
    }

    inline void mark_panel()
    {
        for (uint32_t i = 0; i < DIRTY_WORDS; i++)
            dirty[i] = ~0ULL;
    }

    inline void mark_all()
    {
        if (S == shown)
            mark_panel();
    }

    inline void mark_range(uint32_t from, uint32_t to)
    {
        for (uint32_t y = from; y <= to && y < grid_rows; y++)
            mark_row(y);
    }

    // A blank cell in the colours currently selected, so an erase inside a
    // coloured region does not punch a black hole in it.
    inline void blank(term_cell* c)
    {
        c->ch = ' ';
        c->fg = S->fg;
        c->bg = S->bg;
        c->attr = 0;
    }

    void blank_row(uint32_t y)
    {
        if (y >= grid_rows)
            return;
        term_cell* r = cell_at(0, y);
        for (uint32_t x = 0; x < stride; x++)
            blank(&r[x]);
        mark_row(y);
    }

    // Move the screen up by one line; a blank one appears at the bottom.
    // Leaves the cursor alone - callers decide what it should do.
    void scroll_screen()
    {
        if (!cur_rows)
            return;
        for (uint32_t y = 0; y + 1 < cur_rows; y++)
            memory::memcpy((uint8_t*)cell_at(0, y),
                           (uint8_t*)cell_at(0, y + 1),
                           cur_cols * sizeof(term_cell));
        blank_row(cur_rows - 1);
        mark_range(0, cur_rows - 1);
    }

    // Advance one line, scrolling when the cursor sits on the last row.
    void line_feed()
    {
        if (S->cy + 1 >= cur_rows)
            scroll_screen();
        else if (S->cy + 1 < cur_rows)
            S->cy++;
    }

    void put_glyph(char c)
    {
        if (S->cx >= cur_cols)     // only if something left the cursor stranded
        {
            S->cx = 0;
            line_feed();
        }

        term_cell* p = cell_at(S->cx, S->cy);
        p->ch = (uint8_t)c;
        p->fg = S->fg;
        p->bg = S->bg;
        p->attr = 0;
        mark_row(S->cy);

        // Wrap immediately rather than deferring it to the next glyph. Real
        // terminals defer, but the kernel console's line editor reads
        // cursor_x() back and assumes it is always < cols - with a deferred
        // wrap its backspace would step to the wrong row.
        S->cx++;
        if (S->cx >= cur_cols)
        {
            S->cx = 0;
            line_feed();
        }
    }

    void exec_ctrl(char c)
    {
        switch (c)
        {
            case '\n':
                S->cx = 0;
                line_feed();
                break;
            case '\r':
                S->cx = 0;
                break;
            case '\b':
                // Destructive backspace, matching what the old screen driver
                // did: step back and blank the cell landed on.
                if (S->cx > 0)
                    S->cx--;
                term::erase_at(S->cx, S->cy);
                break;
            case '\t':
                S->cx += 4;
                if (S->cx >= cur_cols)
                {
                    S->cx = 0;
                    line_feed();
                }
                break;
            default:
                break;      // other C0 controls are ignored
        }
    }
}

namespace
{
    // A screen's grid for the whole panel, blank, the cursor at the top.
    bool open_screen(Screen* t)
    {
        uint64_t bytes = (uint64_t)stride * grid_rows * sizeof(term_cell);
        memory::memset((uint8_t*)t, 0, sizeof(Screen));
        t->grid = (term_cell*)kmalloc(bytes);
        if (!t->grid)
        {
            uart::printf("term: cannot allocate a %llu KB cell grid\n", bytes / 1024);
            return false;
        }
        t->fg    = TERM_DEFAULT_FG;
        t->bg    = TERM_DEFAULT_BG;

        S = t;
        term::clear();
        return true;
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

        stride    = panel_cols;
        grid_rows = panel_rows;
        cur_cols  = panel_cols;
        cur_rows  = panel_rows;
        cursor_inverted = false;

        memory::memset((uint8_t*)screens, 0, sizeof(screens));
        for (uint32_t n = 0; n < TERM_SCREENS; n++)
            if (!open_screen(&screens[n]))
                return false;
        S = shown = &screens[0];
        return true;
    }

    sint32_t open_hidden()
    {
        for (uint32_t n = TERM_SCREENS; n < TERM_ALL_SCREENS; n++)
        {
            if (screens[n].grid)
                continue;
            Screen* prev = S;
            bool ok = open_screen(&screens[n]);
            S = prev;
            return ok ? (sint32_t)n : -1;
        }
        return -1;
    }

    void close_hidden(uint32_t n)
    {
        if (n < TERM_SCREENS || n >= TERM_ALL_SCREENS || !screens[n].grid)
            return;
        Screen* t = &screens[n];
        kfree(t->grid);
        if (S == t)
            S = &screens[0];
        memory::memset((uint8_t*)t, 0, sizeof(Screen));
    }

    void copy_screen(uint32_t from, uint32_t to)
    {
        if (from >= TERM_ALL_SCREENS || to >= TERM_ALL_SCREENS || from == to)
            return;
        Screen* f = &screens[from];
        Screen* t = &screens[to];
        if (!f->grid || !t->grid)
            return;
        memory::memcpy((uint8_t*)t->grid, (const uint8_t*)f->grid,
                       (uint64_t)stride * grid_rows * sizeof(term_cell));
        t->cx = f->cx;  t->cy = f->cy;
        t->fg = f->fg;  t->bg = f->bg;
        t->cursor_on = f->cursor_on;
        copy_text(t->subtitle, sizeof(t->subtitle), f->subtitle);
        if (t == shown)
        {
            cursor_inverted = false;
            mark_panel();
            title_dirty = true;
        }
    }

    void resize(uint32_t c, uint32_t r)
    {
        if (!S->grid)
            return;
        if (c > stride)    c = stride;
        if (r > grid_rows) r = grid_rows;
        if (c == 0 || r == 0)
            return;

        cur_cols = c;
        cur_rows = r;
        for (uint32_t n = 0; n < TERM_ALL_SCREENS; n++)
        {
            Screen* t = &screens[n];
            if (!t->grid)
                continue;
            if (t->cx >= cur_cols) t->cx = cur_cols - 1;
            if (t->cy >= cur_rows) t->cy = cur_rows - 1;
        }
        cursor_inverted = false;    // the pixels under it are gone
        invalidate();
    }

    void select(uint32_t n)
    {
        if (n < TERM_ALL_SCREENS && screens[n].grid)
            S = &screens[n];
    }

    uint32_t selected()
    {
        return (uint32_t)(S - screens);
    }

    void show(uint32_t n)
    {
        if (n >= TERM_SCREENS || shown == &screens[n])
            return;
        shown = &screens[n];
        cursor_inverted = false;    // redrawn from the new screen's cells
        mark_panel();
        title_dirty = true;
    }

    uint32_t shown_screen()
    {
        return (uint32_t)(shown - screens);
    }

    void set_program(const char* name)
    {
        copy_text(S->program, sizeof(S->program), name);
        S->subtitle[0] = 0;         // it was the previous program's
        S->paused = false;
        if (S == shown)
            title_dirty = true;
    }

    void set_paused(uint32_t n, bool paused)
    {
        if (n >= TERM_ALL_SCREENS)
            return;
        screens[n].paused = paused;
        if (&screens[n] == shown)
            title_dirty = true;
    }

    void set_status(const char* text, uint32_t ms)
    {
        copy_text(status, sizeof(status), text);
        status_until = text && text[0] && ms ? pit::uptime_ms() + ms : 0;
        title_dirty = true;
    }

    void set_progress(const char* what, uint64_t done, uint64_t total)
    {
        // "what ######.... 2.5/6.0 MB", the bar in block characters.
        const uint32_t BAR = 10;
        char text[64];
        char* p = text;
        char* end = text + sizeof(text) - 1;
        p = append(p, end, what);
        p = append(p, end, " ");
        uint32_t full = total ? (uint32_t)(done * BAR / total) : 0;
        for (uint32_t i = 0; i < BAR && p < end; i++)
            *p++ = (char)(i < full ? 0xDB : 0xB0);       // full block, light shade
        p = append(p, end, " ");
        const uint64_t values[2] = { done, total };
        for (uint32_t k = 0; k < 2; k++)
        {
            uint64_t tenths = values[k] * 10 / (1024 * 1024);
            char num[24];
            uint32_t n = 0;
            uint64_t whole = tenths / 10;
            do
                num[n++] = (char)('0' + whole % 10);
            while ((whole /= 10) && n < 20);
            while (n && p < end)
                *p++ = num[--n];
            if (p + 2 < end)
            {
                *p++ = '.';
                *p++ = (char)('0' + tenths % 10);
            }
            if (k == 0 && p < end)
                *p++ = '/';
        }
        p = append(p, end, " MB");
        *p = 0;
        set_status(text, 0);
    }

    void set_subtitle(const char* text)
    {
        copy_text(S->subtitle, sizeof(S->subtitle), text);
        if (S == shown)
            title_dirty = true;
    }

    uint32_t cols() { return cur_cols; }
    uint32_t rows() { return cur_rows; }

    void putc(char c)
    {
        if (!S->grid)
            return;

        if ((uint8_t)c < 0x20)
            exec_ctrl(c);
        else
            put_glyph(c);
    }

    void feed(const char* s, uint64_t len)
    {
        for (uint64_t i = 0; i < len; i++)
            putc(s[i]);
    }

    void clear()
    {
        if (!S->grid)
            return;
        for (uint32_t y = 0; y < grid_rows; y++)
            blank_row(y);
        S->cx = S->cy = 0;
        if (S == shown)
            cursor_inverted = false;
        mark_all();
    }

    void scroll_up()
    {
        scroll_screen();
        S->cx = 0;
        S->cy = cur_rows ? cur_rows - 1 : 0;
        if (S == shown)
            cursor_inverted = false;
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        if (!S->grid || x >= cur_cols || y >= cur_rows)
            return;
        blank(cell_at(x, y));
        mark_row(y);
    }

    void set_cursor(uint32_t x, uint32_t y)
    {
        if (!S->grid)
            return;
        if (x >= cur_cols) x = cur_cols ? cur_cols - 1 : 0;
        if (y >= cur_rows) y = cur_rows ? cur_rows - 1 : 0;
        S->cx = x;
        S->cy = y;
    }

    uint32_t cursor_x() { return S->cx; }
    uint32_t cursor_y() { return S->cy; }

    void show_cursor() { S->cursor_on = true; }
    void hide_cursor() { S->cursor_on = false; }

    void set_fg(uint8_t idx) { S->fg = idx & 0x0F; }

    void set_colors(uint8_t fg, uint8_t bg)
    {
        S->fg = fg & 0x0F;
        S->bg = bg & 0x0F;
    }

    void put_cell(uint32_t x, uint32_t y, char ch, uint8_t fg, uint8_t bg)
    {
        if (!S->grid || x >= cur_cols || y >= cur_rows)
            return;
        term_cell* c = cell_at(x, y);
        c->ch   = (uint8_t)ch;
        c->fg   = fg & 0x0F;
        c->bg   = bg & 0x0F;
        c->attr = 0;
        mark_row(y);
    }

    void write_at(uint32_t x, uint32_t y, const char* s, uint64_t len)
    {
        for (uint64_t i = 0; i < len && x + i < cur_cols; i++)
            put_cell(x + (uint32_t)i, y, s[i], S->fg, S->bg);
    }

    void invalidate()
    {
        mark_panel();
        title_dirty = true;
    }

    void render()
    {
        if (!shown->grid)
            return;

        draw_title();

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
            term_cell* r = &shown->grid[y * stride];
            for (uint32_t x = 0; x < cur_cols; x++)
                screen::draw_cell(x, y, r[x].ch, r[x].fg, r[x].bg, r[x].attr);
            dirty[y >> 6] &= ~(1ULL << (y & 63));
        }

        // Blink at 500 ms, same rate the old driver used.
        if (shown->cursor_on && shown->cx < cur_cols && shown->cy < cur_rows)
        {
            uint32_t now = (uint32_t)pit::uptime_ms();
            if (((now / 500) % 2) == 0)
            {
                screen::invert_cell(shown->cx, shown->cy);
                cursor_inverted = true;
                inv_x = shown->cx;
                inv_y = shown->cy;
            }
        }
    }
}
