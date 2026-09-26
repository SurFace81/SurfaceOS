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
#include "../../include/drivers/tty.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
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

    enum class St : uint8_t { Ground, Esc, CsiParam, OscString, Discard };
    const uint32_t MAX_PARAMS = 16;

    // One screen: its cells, cursor, colours and escape parser. Output goes
    // to the selected one (S); only the shown one is drawn.
    struct Screen
    {
        term_cell* grid;            // active grid, stride columns per row
        term_cell* main_grid;
        term_cell* alt_grid;        // allocated on first use of ?1049h
        bool       on_alt;

        uint32_t   cx, cy;          // cursor, in cells
        uint8_t    fg, bg, attr;

        // Scroll region, inclusive row indices. Defaults to the whole screen.
        uint32_t   s_top, s_bot;

        // DECSC / DECRC
        uint32_t   sv_x, sv_y;
        uint8_t    sv_fg, sv_bg, sv_attr;

        bool       cursor_on;       // visible at all

        // --- parser -------------------------------------------------------
        St         st;
        uint32_t   params[MAX_PARAMS];
        uint32_t   nparams;
        bool       param_seen;      // distinguishes "ESC[m" from "ESC[0m"
        char       priv;            // '?', '>' or '<' right after the '['
    };

    Screen  screens[TERM_SCREENS];
    Screen* S     = &screens[0];    // output goes here
    Screen* shown = &screens[0];    // on the panel

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
        if (n == 0 || S->s_bot < S->s_top)
            return;
        uint32_t height = S->s_bot - S->s_top + 1;
        if (n >= height)
        {
            for (uint32_t y = S->s_top; y <= S->s_bot; y++)
                blank_row(y);
            return;
        }
        for (uint32_t y = S->s_top; y + n <= S->s_bot; y++)
            memory::memcpy((uint8_t*)cell_at(0, y),
                           (uint8_t*)cell_at(0, y + n),
                           cur_cols * sizeof(term_cell));
        for (uint32_t y = S->s_bot - n + 1; y <= S->s_bot; y++)
            blank_row(y);
        mark_range(S->s_top, S->s_bot);
    }

    // Move the scroll region down by n lines; blanks appear at the top.
    void region_down(uint32_t n)
    {
        if (n == 0 || S->s_bot < S->s_top)
            return;
        uint32_t height = S->s_bot - S->s_top + 1;
        if (n >= height)
        {
            for (uint32_t y = S->s_top; y <= S->s_bot; y++)
                blank_row(y);
            return;
        }
        for (uint32_t y = S->s_bot; y >= S->s_top + n; y--)
            memory::memcpy((uint8_t*)cell_at(0, y),
                           (uint8_t*)cell_at(0, y - n),
                           cur_cols * sizeof(term_cell));
        for (uint32_t y = S->s_top; y < S->s_top + n; y++)
            blank_row(y);
        mark_range(S->s_top, S->s_bot);
    }

    // Advance one line, scrolling the region when the cursor sits on its
    // last row.
    void line_feed()
    {
        if (S->cy == S->s_bot)
            region_up(1);
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
        p->attr = S->attr;
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

    // --- CSI dispatch -----------------------------------------------------

    inline uint32_t param(uint32_t i, uint32_t def)
    {
        if (i >= S->nparams)
            return def;
        return S->params[i] ? S->params[i] : def;
    }

    void set_scroll_region(uint32_t top, uint32_t bot)
    {
        if (bot > cur_rows || bot == 0)
            bot = cur_rows;
        if (top == 0)
            top = 1;
        if (top >= bot)     // an inverted or empty region resets it
        {
            S->s_top = 0;
            S->s_bot = cur_rows ? cur_rows - 1 : 0;
            return;
        }
        S->s_top = top - 1;
        S->s_bot = bot - 1;
    }

    // A cell holds 4 bits of colour, so the extended forms are folded onto
    // the 16 we have. What matters is that their parameters are *consumed*:
    // left in the loop, the trailing components of "38;2;255;0;0" would be
    // read as SGR 0 and reset everything the sequence was setting.
    uint8_t rgb_to_16(uint32_t r, uint32_t g, uint32_t b)
    {
        uint8_t idx = (uint8_t)((r >= 96 ? 1 : 0) | (g >= 96 ? 2 : 0) | (b >= 96 ? 4 : 0));
        uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (mx >= 192)
            idx |= 8;
        return idx;
    }

    uint8_t c256_to_16(uint32_t n)
    {
        if (n < 16)
            return (uint8_t)n;
        if (n >= 232)                       // 24-step greyscale ramp
        {
            uint32_t v = (n - 232) * 10 + 8;
            return rgb_to_16(v, v, v);
        }
        uint32_t c = n - 16;                // 6x6x6 colour cube
        return rgb_to_16(((c / 36) % 6) * 51, ((c / 6) % 6) * 51, (c % 6) * 51);
    }

    // 38/48: `5;N` (256 colours) or `2;R;G;B` (truecolor). Returns how many
    // parameters after the 38/48 were consumed.
    uint32_t extended_colour(uint32_t i, uint8_t* out)
    {
        if (i + 1 >= S->nparams)
            return 0;
        if (S->params[i + 1] == 5 && i + 2 < S->nparams)
        {
            *out = c256_to_16(S->params[i + 2]);
            return 2;
        }
        if (S->params[i + 1] == 2 && i + 4 < S->nparams)
        {
            *out = rgb_to_16(S->params[i + 2], S->params[i + 3], S->params[i + 4]);
            return 4;
        }
        return 1;                           // malformed: swallow the selector
    }

    void do_sgr()
    {
        if (S->nparams == 0 || !S->param_seen)
        {
            S->fg = TERM_DEFAULT_FG;
            S->bg = TERM_DEFAULT_BG;
            S->attr = 0;
            return;
        }
        for (uint32_t i = 0; i < S->nparams; i++)
        {
            uint32_t p = S->params[i];
            if (p == 0)                  { S->fg = TERM_DEFAULT_FG;
                                           S->bg = TERM_DEFAULT_BG;
                                           S->attr = 0; }
            else if (p == 1)             S->attr |= TERM_BOLD;
            else if (p == 7)             S->attr |= TERM_REVERSE;
            else if (p == 22)            S->attr &= ~TERM_BOLD;
            else if (p == 27)            S->attr &= ~TERM_REVERSE;
            else if (p >= 30 && p <= 37) S->fg = (uint8_t)(p - 30);
            else if (p == 39)            S->fg = TERM_DEFAULT_FG;
            else if (p >= 40 && p <= 47) S->bg = (uint8_t)(p - 40);
            else if (p == 49)            S->bg = TERM_DEFAULT_BG;
            else if (p >= 90 && p <= 97) S->fg = (uint8_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) S->bg = (uint8_t)(p - 100 + 8);
            else if (p == 38)            i += extended_colour(i, &S->fg);
            else if (p == 48)            i += extended_colour(i, &S->bg);
        }
    }

    void erase_display(uint32_t mode)
    {
        if (mode == 0)                          // cursor to end
        {
            blank_cells(S->cx, S->cy, cur_cols - S->cx);
            for (uint32_t y = S->cy + 1; y < cur_rows; y++)
                blank_row(y);
        }
        else if (mode == 1)                     // start to cursor
        {
            for (uint32_t y = 0; y < S->cy; y++)
                blank_row(y);
            blank_cells(0, S->cy, S->cx + 1);
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
            blank_cells(S->cx, S->cy, cur_cols - S->cx);
        else if (mode == 1)
            blank_cells(0, S->cy, S->cx + 1);
        else
            blank_cells(0, S->cy, cur_cols);
    }

    // Insert/delete lines at the cursor, confined to the scroll region.
    void insert_lines(uint32_t n)
    {
        if (S->cy < S->s_top || S->cy > S->s_bot)
            return;
        uint32_t save = S->s_top;
        S->s_top = S->cy;
        region_down(n);
        S->s_top = save;
    }

    void delete_lines(uint32_t n)
    {
        if (S->cy < S->s_top || S->cy > S->s_bot)
            return;
        uint32_t save = S->s_top;
        S->s_top = S->cy;
        region_up(n);
        S->s_top = save;
    }

    void insert_chars(uint32_t n)
    {
        if (S->cy >= cur_rows || S->cx >= cur_cols)
            return;
        if (n > cur_cols - S->cx)
            n = cur_cols - S->cx;
        term_cell* r = cell_at(0, S->cy);
        for (uint32_t x = cur_cols; x-- > S->cx + n; )
            r[x] = r[x - n];
        for (uint32_t x = S->cx; x < S->cx + n; x++)
            blank(&r[x]);
        mark_row(S->cy);
    }

    void delete_chars(uint32_t n)
    {
        if (S->cy >= cur_rows || S->cx >= cur_cols)
            return;
        if (n > cur_cols - S->cx)
            n = cur_cols - S->cx;
        term_cell* r = cell_at(0, S->cy);
        for (uint32_t x = S->cx; x + n < cur_cols; x++)
            r[x] = r[x + n];
        for (uint32_t x = cur_cols - n; x < cur_cols; x++)
            blank(&r[x]);
        mark_row(S->cy);
    }

    void save_cursor()
    {
        S->sv_x = S->cx; S->sv_y = S->cy;
        S->sv_fg = S->fg; S->sv_bg = S->bg; S->sv_attr = S->attr;
    }

    void restore_cursor()
    {
        S->cx = S->sv_x < cur_cols ? S->sv_x : (cur_cols ? cur_cols - 1 : 0);
        S->cy = S->sv_y < cur_rows ? S->sv_y : (cur_rows ? cur_rows - 1 : 0);
        S->fg = S->sv_fg; S->bg = S->sv_bg; S->attr = S->sv_attr;
    }

    void switch_alt(bool want_alt)
    {
        if (want_alt == S->on_alt)
            return;

        if (want_alt)
        {
            if (!S->alt_grid)
            {
                uint64_t bytes = (uint64_t)stride * grid_rows * sizeof(term_cell);
                S->alt_grid = (term_cell*)kmalloc(bytes);
                if (!S->alt_grid)
                    return;         // no alt screen: stay where we are
            }
            save_cursor();
            S->grid = S->alt_grid;
            S->on_alt = true;
            for (uint32_t y = 0; y < grid_rows; y++)
                blank_row(y);
            S->cx = S->cy = 0;
        }
        else
        {
            S->grid = S->main_grid;
            S->on_alt = false;
            restore_cursor();
        }
        mark_all();
    }

    void set_mode(bool on)
    {
        for (uint32_t i = 0; i < S->nparams; i++)
        {
            if (S->priv == '?')
            {
                switch (S->params[i])
                {
                    case 25:                    // DECTCEM
                        S->cursor_on = on;
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
                        S->cy = (S->cy > n) ? S->cy - n : 0; } break;
            case 'B': { uint32_t n = param(0, 1);
                        S->cy = (S->cy + n < cur_rows) ? S->cy + n : (cur_rows ? cur_rows - 1 : 0); } break;
            case 'C': { uint32_t n = param(0, 1);
                        S->cx = (S->cx + n < cur_cols) ? S->cx + n : (cur_cols ? cur_cols - 1 : 0); } break;
            case 'D': { uint32_t n = param(0, 1);
                        S->cx = (S->cx > n) ? S->cx - n : 0; } break;
            case 'E': { uint32_t n = param(0, 1);
                        S->cy = (S->cy + n < cur_rows) ? S->cy + n : (cur_rows ? cur_rows - 1 : 0);
                        S->cx = 0; } break;
            case 'F': { uint32_t n = param(0, 1);
                        S->cy = (S->cy > n) ? S->cy - n : 0;
                        S->cx = 0; } break;
            case 'G': term::set_cursor(param(0, 1) - 1, S->cy); break;
            case 'd': term::set_cursor(S->cx, param(0, 1) - 1); break;
            case 'H':
            case 'f': term::set_cursor(param(1, 1) - 1, param(0, 1) - 1); break;
            // J and K take 0 as a real mode, not "use the default".
            case 'J': erase_display(S->nparams ? S->params[0] : 0); break;
            case 'K': erase_line(S->nparams ? S->params[0] : 0); break;
            case 'L': insert_lines(param(0, 1)); break;
            case 'M': delete_lines(param(0, 1)); break;
            case 'P': delete_chars(param(0, 1)); break;
            case '@': insert_chars(param(0, 1)); break;
            case 'X': blank_cells(S->cx, S->cy, param(0, 1)); break;
            case 'S': region_up(param(0, 1)); break;
            case 'T': region_down(param(0, 1)); break;
            // DECSTBM. The private form (ESC [ ? Ps r, DECRSTR) restores
            // saved modes and must not be mistaken for a scroll region.
            case 'r': if (S->priv) break;
                      set_scroll_region(param(0, 1), S->nparams > 1 ? S->params[1] : 0);
                      S->cx = 0; S->cy = S->s_top; break;
            case 'm': do_sgr(); break;
            case 'h': set_mode(true); break;
            case 'l': set_mode(false); break;
            case 's': save_cursor(); break;
            case 'u':
                // ESC [ > 1 u / ESC [ < u toggle full-fidelity key
                // reporting, which is an input-side setting: the terminal
                // only relays it. Without a private marker this is SCORC.
                if (S->priv == '>')      tty::set_csi_u(param(0, 1) != 0);
                else if (S->priv == '<') tty::set_csi_u(false);
                else                  restore_cursor();
                break;
            default: break;
        }
    }

    void csi_reset()
    {
        S->nparams = 0;
        S->param_seen = false;
        S->priv = 0;
        for (uint32_t i = 0; i < MAX_PARAMS; i++)
            S->params[i] = 0;
    }

    void full_reset()
    {
        S->fg = TERM_DEFAULT_FG;
        S->bg = TERM_DEFAULT_BG;
        S->attr = 0;
        S->s_top = 0;
        S->s_bot = cur_rows ? cur_rows - 1 : 0;
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

        stride    = panel_cols;
        grid_rows = panel_rows;
        cur_cols  = panel_cols;
        cur_rows  = panel_rows;
        cursor_inverted = false;

        uint64_t bytes = (uint64_t)panel_cols * panel_rows * sizeof(term_cell);
        for (uint32_t n = 0; n < TERM_SCREENS; n++)
        {
            Screen* t = &screens[n];
            memory::memset((uint8_t*)t, 0, sizeof(Screen));
            t->main_grid = (term_cell*)kmalloc(bytes);
            if (!t->main_grid)
            {
                uart::printf("term: cannot allocate a %llu KB cell grid\n",
                             bytes / 1024);
                return false;
            }
            t->grid  = t->main_grid;
            t->fg    = TERM_DEFAULT_FG;
            t->bg    = TERM_DEFAULT_BG;
            t->s_bot = cur_rows - 1;
            t->st    = St::Ground;

            S = t;
            clear();
        }
        S = shown = &screens[0];
        return true;
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
        for (uint32_t n = 0; n < TERM_SCREENS; n++)
        {
            Screen* t = &screens[n];
            if (t->cx >= cur_cols) t->cx = cur_cols - 1;
            if (t->cy >= cur_rows) t->cy = cur_rows - 1;
            // A resize invalidates any scroll region the old geometry defined.
            t->s_top = 0;
            t->s_bot = cur_rows - 1;
        }
        cursor_inverted = false;    // the pixels under it are gone
        invalidate();
    }

    void select(uint32_t n)
    {
        if (n < TERM_SCREENS)
            S = &screens[n];
    }

    void show(uint32_t n)
    {
        if (n >= TERM_SCREENS || shown == &screens[n])
            return;
        shown = &screens[n];
        cursor_inverted = false;    // redrawn from the new screen's cells
        mark_panel();
    }

    uint32_t shown_screen()
    {
        return (uint32_t)(shown - screens);
    }

    uint32_t cols() { return cur_cols; }
    uint32_t rows() { return cur_rows; }

    void putc(char c)
    {
        if (!S->grid)
            return;

        switch (S->st)
        {
            case St::Ground:
                if (c == 0x1B)
                {
                    S->st = St::Esc;
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
                    case '[': S->st = St::CsiParam; return;
                    case ']': S->st = St::OscString; return;
                    case '7': save_cursor();    S->st = St::Ground; return;
                    case '8': restore_cursor(); S->st = St::Ground; return;
                    case 'M':                   // RI: up, scrolling at the top
                        if (S->cy == S->s_top) region_down(1);
                        else if (S->cy > 0) S->cy--;
                        S->st = St::Ground;
                        return;
                    case 'c': full_reset(); S->st = St::Ground; return;
                    case '(': case ')': case '*': case '+':
                        S->st = St::Discard;       // charset select: eat one byte
                        return;
                    default:
                        S->st = St::Ground;
                        return;
                }

            case St::CsiParam:
                if (c >= '0' && c <= '9')
                {
                    if (S->nparams == 0)
                        S->nparams = 1;
                    uint32_t& p = S->params[S->nparams - 1];
                    // Clamp instead of overflowing on a long digit run.
                    if (p < 100000)
                        p = p * 10 + (uint32_t)(c - '0');
                    S->param_seen = true;
                    return;
                }
                if (c == ';')
                {
                    if (S->nparams == 0)
                        S->nparams = 1;
                    if (S->nparams < MAX_PARAMS)
                        S->nparams++;
                    S->param_seen = true;
                    return;
                }
                if ((c == '?' || c == '>' || c == '<' || c == '!') && S->nparams == 0)
                {
                    S->priv = c;
                    return;
                }
                if (c >= 0x20 && c <= 0x2F)
                    return;                     // intermediate bytes: ignored
                if (c >= 0x40 && c <= 0x7E)
                {
                    dispatch_csi(c);
                    S->st = St::Ground;
                    csi_reset();
                    return;
                }
                if (c == 0x1B)
                {
                    // A fresh ESC abandons this sequence and starts the
                    // next one, rather than being swallowed as a stray
                    // byte - that would print the rest of it as text.
                    S->st = St::Esc;
                    csi_reset();
                    return;
                }
                // Anything else aborts the sequence rather than hanging on it.
                S->st = St::Ground;
                csi_reset();
                return;

            case St::OscString:
                // Title strings and friends: swallow until BEL or ESC \.
                if (c == 0x07)
                    S->st = St::Ground;
                else if (c == 0x1B)
                    S->st = St::Discard;
                return;

            case St::Discard:
                S->st = St::Ground;
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
        region_up(1);
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

    void invalidate()
    {
        mark_panel();
    }

    void render()
    {
        if (!shown->grid)
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
