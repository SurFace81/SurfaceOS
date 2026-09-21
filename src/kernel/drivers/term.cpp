// Terminal emulator: the cell grid, the cursor and the escape parser.
// See term.h for why this is split from screen.cpp.
//
// The parser is a small state machine rather than a scan of the incoming
// buffer, because a sequence can be split across two feed() calls: the
// write path chops user data into BOUNCE_SIZE chunks, so "ESC [ 3 1 m" can
// arrive as "ESC [ 3" and then "1 m". All of its state therefore lives in
// this module, never on the stack.

#include "../../include/drivers/term.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"

namespace
{
    term_cell* grid = nullptr;      // active grid, stride columns per row
    term_cell* main_grid = nullptr;
    term_cell* alt_grid = nullptr;  // allocated on first use of ?1049h
    bool       on_alt = false;

    uint32_t   stride = 0;          // allocated columns per row
    uint32_t   grid_rows = 0;       // allocated rows

    uint32_t   cur_cols = 0;        // active area (follows the viewport)
    uint32_t   cur_rows = 0;

    uint32_t   cx = 0, cy = 0;      // cursor, in cells
    uint8_t    fg = TERM_DEFAULT_FG;
    uint8_t    bg = TERM_DEFAULT_BG;
    uint8_t    attr = 0;

    // Scroll region, inclusive row indices. Defaults to the whole screen.
    uint32_t   s_top = 0;
    uint32_t   s_bot = 0;

    // DECSC / DECRC
    uint32_t   sv_x = 0, sv_y = 0;
    uint8_t    sv_fg = TERM_DEFAULT_FG, sv_bg = TERM_DEFAULT_BG, sv_attr = 0;

    bool       cursor_on = false;   // visible at all
    bool       cursor_inverted = false;
    uint32_t   inv_x = 0, inv_y = 0;    // where the inversion currently sits

    // One dirty bit per row. 64 rows per word; a 4K panel is 135 rows.
    const uint32_t DIRTY_WORDS = 8;     // up to 512 rows
    uint64_t   dirty[DIRTY_WORDS];

    // --- parser -----------------------------------------------------------
    enum class St : uint8_t { Ground, Esc, CsiParam, OscString, Discard };

    const uint32_t MAX_PARAMS = 16;
    St       st = St::Ground;
    uint32_t params[MAX_PARAMS];
    uint32_t nparams = 0;
    bool     param_seen = false;    // distinguishes "ESC[m" from "ESC[0m"
    char     priv = 0;              // '?', '>' or '<' right after the '['

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
        c->fg = fg;
        c->bg = bg;
        c->attr = 0;
    }

    void blank_cells(uint32_t x, uint32_t y, uint32_t n)
    {
        if (y >= cur_rows)
            return;
        term_cell* r = cell_at(0, y);
        for (uint32_t i = x; i < x + n && i < cur_cols; i++)
            blank(&r[i]);
        mark_row(y);
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

    // Move the scroll region up by n lines; blanks appear at the bottom.
    // Leaves the cursor alone - callers decide what it should do.
    void region_up(uint32_t n)
    {
        if (n == 0 || s_bot < s_top)
            return;
        uint32_t height = s_bot - s_top + 1;
        if (n >= height)
        {
            for (uint32_t y = s_top; y <= s_bot; y++)
                blank_row(y);
            return;
        }
        for (uint32_t y = s_top; y + n <= s_bot; y++)
            memory::memcpy((uint8_t*)cell_at(0, y),
                           (uint8_t*)cell_at(0, y + n),
                           cur_cols * sizeof(term_cell));
        for (uint32_t y = s_bot - n + 1; y <= s_bot; y++)
            blank_row(y);
        mark_range(s_top, s_bot);
    }

    // Move the scroll region down by n lines; blanks appear at the top.
    void region_down(uint32_t n)
    {
        if (n == 0 || s_bot < s_top)
            return;
        uint32_t height = s_bot - s_top + 1;
        if (n >= height)
        {
            for (uint32_t y = s_top; y <= s_bot; y++)
                blank_row(y);
            return;
        }
        for (uint32_t y = s_bot; y >= s_top + n; y--)
            memory::memcpy((uint8_t*)cell_at(0, y),
                           (uint8_t*)cell_at(0, y - n),
                           cur_cols * sizeof(term_cell));
        for (uint32_t y = s_top; y < s_top + n; y++)
            blank_row(y);
        mark_range(s_top, s_bot);
    }

    // Advance one line, scrolling the region when the cursor sits on its
    // last row.
    void line_feed()
    {
        if (cy == s_bot)
            region_up(1);
        else if (cy + 1 < cur_rows)
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

    void exec_ctrl(char c)
    {
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
                term::erase_at(cx, cy);
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
                break;      // other C0 controls are ignored
        }
    }

    // --- CSI dispatch -----------------------------------------------------

    inline uint32_t param(uint32_t i, uint32_t def)
    {
        if (i >= nparams)
            return def;
        return params[i] ? params[i] : def;
    }

    void set_scroll_region(uint32_t top, uint32_t bot)
    {
        if (bot > cur_rows || bot == 0)
            bot = cur_rows;
        if (top == 0)
            top = 1;
        if (top >= bot)     // an inverted or empty region resets it
        {
            s_top = 0;
            s_bot = cur_rows ? cur_rows - 1 : 0;
            return;
        }
        s_top = top - 1;
        s_bot = bot - 1;
    }

    void do_sgr()
    {
        if (nparams == 0 || !param_seen)
        {
            fg = TERM_DEFAULT_FG;
            bg = TERM_DEFAULT_BG;
            attr = 0;
            return;
        }
        for (uint32_t i = 0; i < nparams; i++)
        {
            uint32_t p = params[i];
            if (p == 0)                  { fg = TERM_DEFAULT_FG;
                                           bg = TERM_DEFAULT_BG;
                                           attr = 0; }
            else if (p == 1)             attr |= TERM_BOLD;
            else if (p == 7)             attr |= TERM_REVERSE;
            else if (p == 22)            attr &= ~TERM_BOLD;
            else if (p == 27)            attr &= ~TERM_REVERSE;
            else if (p >= 30 && p <= 37) fg = (uint8_t)(p - 30);
            else if (p == 39)            fg = TERM_DEFAULT_FG;
            else if (p >= 40 && p <= 47) bg = (uint8_t)(p - 40);
            else if (p == 49)            bg = TERM_DEFAULT_BG;
            else if (p >= 90 && p <= 97) fg = (uint8_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) bg = (uint8_t)(p - 100 + 8);
        }
    }

    void erase_display(uint32_t mode)
    {
        if (mode == 0)                          // cursor to end
        {
            blank_cells(cx, cy, cur_cols - cx);
            for (uint32_t y = cy + 1; y < cur_rows; y++)
                blank_row(y);
        }
        else if (mode == 1)                     // start to cursor
        {
            for (uint32_t y = 0; y < cy; y++)
                blank_row(y);
            blank_cells(0, cy, cx + 1);
        }
        else                                    // 2 and 3: everything
        {
            for (uint32_t y = 0; y < cur_rows; y++)
                blank_row(y);
        }
    }

    void erase_line(uint32_t mode)
    {
        if (mode == 0)
            blank_cells(cx, cy, cur_cols - cx);
        else if (mode == 1)
            blank_cells(0, cy, cx + 1);
        else
            blank_cells(0, cy, cur_cols);
    }

    // Insert/delete lines at the cursor, confined to the scroll region.
    void insert_lines(uint32_t n)
    {
        if (cy < s_top || cy > s_bot)
            return;
        uint32_t save = s_top;
        s_top = cy;
        region_down(n);
        s_top = save;
    }

    void delete_lines(uint32_t n)
    {
        if (cy < s_top || cy > s_bot)
            return;
        uint32_t save = s_top;
        s_top = cy;
        region_up(n);
        s_top = save;
    }

    void insert_chars(uint32_t n)
    {
        if (cy >= cur_rows || cx >= cur_cols)
            return;
        if (n > cur_cols - cx)
            n = cur_cols - cx;
        term_cell* r = cell_at(0, cy);
        for (uint32_t x = cur_cols; x-- > cx + n; )
            r[x] = r[x - n];
        for (uint32_t x = cx; x < cx + n; x++)
            blank(&r[x]);
        mark_row(cy);
    }

    void delete_chars(uint32_t n)
    {
        if (cy >= cur_rows || cx >= cur_cols)
            return;
        if (n > cur_cols - cx)
            n = cur_cols - cx;
        term_cell* r = cell_at(0, cy);
        for (uint32_t x = cx; x + n < cur_cols; x++)
            r[x] = r[x + n];
        for (uint32_t x = cur_cols - n; x < cur_cols; x++)
            blank(&r[x]);
        mark_row(cy);
    }

    void save_cursor()
    {
        sv_x = cx; sv_y = cy;
        sv_fg = fg; sv_bg = bg; sv_attr = attr;
    }

    void restore_cursor()
    {
        cx = sv_x < cur_cols ? sv_x : (cur_cols ? cur_cols - 1 : 0);
        cy = sv_y < cur_rows ? sv_y : (cur_rows ? cur_rows - 1 : 0);
        fg = sv_fg; bg = sv_bg; attr = sv_attr;
    }

    void switch_alt(bool want_alt)
    {
        if (want_alt == on_alt)
            return;

        if (want_alt)
        {
            if (!alt_grid)
            {
                uint64_t bytes = (uint64_t)stride * grid_rows * sizeof(term_cell);
                alt_grid = (term_cell*)kmalloc(bytes);
                if (!alt_grid)
                    return;         // no alt screen: stay where we are
            }
            save_cursor();
            grid = alt_grid;
            on_alt = true;
            for (uint32_t y = 0; y < grid_rows; y++)
                blank_row(y);
            cx = cy = 0;
        }
        else
        {
            grid = main_grid;
            on_alt = false;
            restore_cursor();
        }
        mark_all();
    }

    void set_mode(bool on)
    {
        for (uint32_t i = 0; i < nparams; i++)
        {
            if (priv == '?')
            {
                switch (params[i])
                {
                    case 25:                    // DECTCEM
                        cursor_on = on;
                        break;
                    case 1047:
                    case 1049:
                        switch_alt(on);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    void dispatch_csi(char final)
    {
        switch (final)
        {
            case 'A': { uint32_t n = param(0, 1);
                        cy = (cy > n) ? cy - n : 0; } break;
            case 'B': { uint32_t n = param(0, 1);
                        cy = (cy + n < cur_rows) ? cy + n : (cur_rows ? cur_rows - 1 : 0); } break;
            case 'C': { uint32_t n = param(0, 1);
                        cx = (cx + n < cur_cols) ? cx + n : (cur_cols ? cur_cols - 1 : 0); } break;
            case 'D': { uint32_t n = param(0, 1);
                        cx = (cx > n) ? cx - n : 0; } break;
            case 'E': { uint32_t n = param(0, 1);
                        cy = (cy + n < cur_rows) ? cy + n : (cur_rows ? cur_rows - 1 : 0);
                        cx = 0; } break;
            case 'F': { uint32_t n = param(0, 1);
                        cy = (cy > n) ? cy - n : 0;
                        cx = 0; } break;
            case 'G': term::set_cursor(param(0, 1) - 1, cy); break;
            case 'd': term::set_cursor(cx, param(0, 1) - 1); break;
            case 'H':
            case 'f': term::set_cursor(param(1, 1) - 1, param(0, 1) - 1); break;
            // J and K take 0 as a real mode, not "use the default".
            case 'J': erase_display(nparams ? params[0] : 0); break;
            case 'K': erase_line(nparams ? params[0] : 0); break;
            case 'L': insert_lines(param(0, 1)); break;
            case 'M': delete_lines(param(0, 1)); break;
            case 'P': delete_chars(param(0, 1)); break;
            case '@': insert_chars(param(0, 1)); break;
            case 'X': blank_cells(cx, cy, param(0, 1)); break;
            case 'S': region_up(param(0, 1)); break;
            case 'T': region_down(param(0, 1)); break;
            case 'r': set_scroll_region(param(0, 1), nparams > 1 ? params[1] : 0);
                      cx = 0; cy = s_top; break;
            case 'm': do_sgr(); break;
            case 'h': set_mode(true); break;
            case 'l': set_mode(false); break;
            case 's': save_cursor(); break;
            case 'u': restore_cursor(); break;
            default: break;
        }
    }

    void csi_reset()
    {
        nparams = 0;
        param_seen = false;
        priv = 0;
        for (uint32_t i = 0; i < MAX_PARAMS; i++)
            params[i] = 0;
    }

    void full_reset()
    {
        fg = TERM_DEFAULT_FG;
        bg = TERM_DEFAULT_BG;
        attr = 0;
        s_top = 0;
        s_bot = cur_rows ? cur_rows - 1 : 0;
        switch_alt(false);
        term::clear();
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
        main_grid = (term_cell*)kmalloc(bytes);
        if (!main_grid)
        {
            uart::printf("term: cannot allocate a %llu KB cell grid\n",
                         bytes / 1024);
            return false;
        }

        grid      = main_grid;
        alt_grid  = nullptr;
        on_alt    = false;
        stride    = panel_cols;
        grid_rows = panel_rows;
        cur_cols  = panel_cols;
        cur_rows  = panel_rows;

        cx = cy = 0;
        fg = TERM_DEFAULT_FG;
        bg = TERM_DEFAULT_BG;
        attr = 0;
        s_top = 0;
        s_bot = cur_rows - 1;
        cursor_on = false;
        cursor_inverted = false;
        st = St::Ground;
        csi_reset();

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
        if (cx >= cur_cols) cx = cur_cols - 1;
        if (cy >= cur_rows) cy = cur_rows - 1;
        // A resize invalidates any scroll region the old geometry defined.
        s_top = 0;
        s_bot = cur_rows - 1;
        cursor_inverted = false;    // the pixels under it are gone
        invalidate();
    }

    uint32_t cols() { return cur_cols; }
    uint32_t rows() { return cur_rows; }

    void putc(char c)
    {
        if (!grid)
            return;

        switch (st)
        {
            case St::Ground:
                if (c == 0x1B)
                {
                    st = St::Esc;
                    csi_reset();
                }
                else if ((uint8_t)c < 0x20)
                    exec_ctrl(c);
                else
                    put_glyph(c);
                return;

            case St::Esc:
                switch (c)
                {
                    case '[': st = St::CsiParam; return;
                    case ']': st = St::OscString; return;
                    case '7': save_cursor();    st = St::Ground; return;
                    case '8': restore_cursor(); st = St::Ground; return;
                    case 'M':                   // RI: up, scrolling at the top
                        if (cy == s_top) region_down(1);
                        else if (cy > 0) cy--;
                        st = St::Ground;
                        return;
                    case 'c': full_reset(); st = St::Ground; return;
                    case '(': case ')': case '*': case '+':
                        st = St::Discard;       // charset select: eat one byte
                        return;
                    default:
                        st = St::Ground;
                        return;
                }

            case St::CsiParam:
                if (c >= '0' && c <= '9')
                {
                    if (nparams == 0)
                        nparams = 1;
                    uint32_t& p = params[nparams - 1];
                    // Clamp instead of overflowing on a long digit run.
                    if (p < 100000)
                        p = p * 10 + (uint32_t)(c - '0');
                    param_seen = true;
                    return;
                }
                if (c == ';')
                {
                    if (nparams == 0)
                        nparams = 1;
                    if (nparams < MAX_PARAMS)
                        nparams++;
                    param_seen = true;
                    return;
                }
                if ((c == '?' || c == '>' || c == '<' || c == '!') && nparams == 0)
                {
                    priv = c;
                    return;
                }
                if (c >= 0x20 && c <= 0x2F)
                    return;                     // intermediate bytes: ignored
                if (c >= 0x40 && c <= 0x7E)
                {
                    dispatch_csi(c);
                    st = St::Ground;
                    csi_reset();
                    return;
                }
                if (c == 0x1B)
                {
                    // A fresh ESC abandons this sequence and starts the
                    // next one, rather than being swallowed as a stray
                    // byte - that would print the rest of it as text.
                    st = St::Esc;
                    csi_reset();
                    return;
                }
                // Anything else aborts the sequence rather than hanging on it.
                st = St::Ground;
                csi_reset();
                return;

            case St::OscString:
                // Title strings and friends: swallow until BEL or ESC \.
                if (c == 0x07)
                    st = St::Ground;
                else if (c == 0x1B)
                    st = St::Discard;
                return;

            case St::Discard:
                st = St::Ground;
                return;
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
        region_up(1);
        cx = 0;
        cy = cur_rows ? cur_rows - 1 : 0;
        cursor_inverted = false;
    }

    void erase_at(uint32_t x, uint32_t y)
    {
        if (!grid || x >= cur_cols || y >= cur_rows)
            return;
        blank(cell_at(x, y));
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
