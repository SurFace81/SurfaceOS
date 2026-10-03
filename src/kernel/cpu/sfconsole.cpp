// The console through the SurfaceOS SDK. See sfconsole.h and
// sfos/console.h.
//
// Every call acts on the caller's own screen (OnScreen below), whichever
// one the panel shows. Keys come from the tty's event ring, which the
// keyboard fills while a program owns the shown screen's input; a reader
// that does not own its screen's input waits for its turn first.

#include "../../include/cpu/sfconsole.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/tty.h"
#include "../../include/mm/heap.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/sfos.h"

namespace
{
    // Output goes to the caller's screen while it draws; the kernel's own
    // output goes on where it went before. Never held across a sleep: the
    // selected screen is everyone's, and whoever runs meanwhile selects
    // their own.
    struct OnScreen
    {
        uint32_t prev;
        OnScreen() : prev(term::selected()) { term::select(process::current_screen()); }
        ~OnScreen() { term::select(prev); }
    };

    const uint64_t TEXT_MAX = 1024;     // one chunk of a string from the program

    // (const char* Text)
    void print(user_regs* regs, iret_frame*)
    {
        char* buf = (char*)kmalloc(TEXT_MAX);
        if (!buf)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        // Any length, a chunk at a time: -2 means the chunk is full and the
        // text goes on. The screen is selected for the writing only: a
        // write into a file may sleep, and meanwhile others select theirs.
        uint64_t text = regs->rdi;
        for (;;)
        {
            sint64_t len = uaccess::strncpy_from_user(buf, text, TEXT_MAX);
            if (len == -1)
            {
                regs->rax = SF_INVALID_PARAMETER;
                break;
            }
            uint64_t n = len >= 0 ? (uint64_t)len : TEXT_MAX - 1;
            if (!process::output_to_file(buf, n))
            {
                {
                    OnScreen on;
                    tty::write(buf, n);
                }
                process::log_output(buf, n);    // in the background: its log
            }
            if (len >= 0)
            {
                regs->rax = SF_SUCCESS;
                break;
            }
            text += TEXT_MAX - 1;
        }
        kfree(buf);
    }

    // The next key press for the caller, once it is its turn. False when the
    // program is being ended, or when another
    // program on its screen took the input meanwhile (Ctrl+Alt+Z letting a
    // paused one go on). Moved to another screen (fg, bg), it waits for its
    // turn there.
    bool next_key(keyboard_event_t* e)
    {
        for (;;)
        {
            if (!process::wait_for_input())
                return false;
            uint32_t screen = process::current_screen();
            while (process::current_screen() == screen)
            {
                if (process::input_changed(screen))
                    return false;
                if (tty::pop_key(e, screen))
                {
                    if (e->type == KEY_PRESS)
                        return true;
                }
                else if (!tty::wait_key(screen, process::input_changed))
                    return false;
            }
        }
    }

    inline bool is_ctrl_c(const keyboard_event_t& e)
    {
        return e.Control && e.KeyChar == 3;
    }

    // A key ReadKey reports: a press, not of a modifier on its own.
    bool reportable(const keyboard_event_t& e)
    {
        return e.type == KEY_PRESS &&
               (e.KeyChar || (e.KeyCode != KEY_LEFT_SHIFT && e.KeyCode != KEY_RIGHT_SHIFT &&
                              e.KeyCode != KEY_LEFT_CTRL && e.KeyCode != KEY_RIGHT_CTRL &&
                              e.KeyCode != KEY_LEFT_ALT && e.KeyCode != KEY_RIGHT_ALT &&
                              e.KeyCode != KEY_CAPS_LOCK && e.KeyCode != KEY_NUM_LOCK &&
                              e.KeyCode != KEY_SCROLL_LOCK));
    }

    // --- the clipboard -------------------------------------------------------
    //
    // One for every program and screen: SetClipboard and GetClipboard, and
    // Ctrl+C and Ctrl+V in ReadLine.

    char*    clip;                  // kmalloc'ed; null while empty
    uint64_t clip_len;

    // Put a copy of `len` bytes of data on it; false when out of memory.
    bool clip_set(const char* data, uint64_t len)
    {
        char* copy = len ? (char*)kmalloc(len) : nullptr;
        if (len && !copy)
            return false;
        memcpy(copy, data, len);
        if (clip)
            kfree(clip);
        clip = copy;
        clip_len = len;
        return true;
    }

    // --- selecting on the screen -------------------------------------------
    //
    // In ReadLine, Ctrl+arrows select the cells from where the cursor was
    // (anchor) to a moving end, row after row as the text reads; shown
    // inverted. Cells are counted y * columns + x; the end's own cell is
    // not in it.

    struct Selection
    {
        bool     on;
        uint32_t anchor;
        uint32_t end;
    };

    // Take the selection off the screen - off every cell, in case something
    // printed meanwhile moved it - and show `s` when it is on.
    void show_selection(const Selection* s)
    {
        uint32_t cols = term::cols(), rows = term::rows();
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < cols; x++)
                term::set_reverse(x, y, false);
        if (!s->on)
            return;
        uint32_t from = s->anchor < s->end ? s->anchor : s->end;
        uint32_t to   = s->anchor < s->end ? s->end : s->anchor;
        for (uint32_t i = from; i < to; i++)
            term::set_reverse(i % cols, i / cols, true);
    }

    // Ctrl+arrow: move the end a cell or a row. False for any other key.
    bool move_selection(Selection* s, const keyboard_event_t& e)
    {
        uint8_t k = e.KeyCode;
        if (!e.Control || e.Alt || (k != KEY_ARROW_LEFT && k != KEY_ARROW_RIGHT &&
                                    k != KEY_ARROW_UP && k != KEY_ARROW_DOWN))
            return false;
        uint32_t cols = term::cols(), cells = cols * term::rows();
        if (!s->on)
        {
            s->on = true;
            s->anchor = s->end = term::cursor_y() * cols + term::cursor_x();
        }
        if (k == KEY_ARROW_LEFT && s->end > 0)
            s->end--;
        else if (k == KEY_ARROW_RIGHT && s->end < cells)
            s->end++;
        else if (k == KEY_ARROW_UP && s->end >= cols)
            s->end -= cols;
        else if (k == KEY_ARROW_DOWN && s->end + cols <= cells)
            s->end += cols;
        show_selection(s);
        return true;
    }

    // The selected text onto the clipboard: each row without the blanks at
    // its end, the rows apart by line breaks.
    void copy_selection(const Selection* s)
    {
        uint32_t cols = term::cols();
        uint32_t from = s->anchor < s->end ? s->anchor : s->end;
        uint32_t to   = s->anchor < s->end ? s->end : s->anchor;
        char* text = (char*)kmalloc(to - from + to / cols + 1);
        if (!text)
            return;
        uint64_t n = 0;
        for (uint32_t i = from; i < to; i++)
        {
            text[n++] = term::char_at(i % cols, i / cols);
            if ((i + 1) % cols == 0 || i + 1 == to)
            {
                while (n && text[n - 1] == ' ')
                    n--;
                if (i + 1 < to)
                    text[n++] = '\n';
            }
        }
        clip_set(text, n);
        kfree(text);
    }

    // --- ReadLine's line editor --------------------------------------------
    //
    // The line is kept in `text`; on the screen it starts where the cursor
    // was and may wrap over several rows. A row count that grows past the
    // bottom scrolls the screen, which moves the start up.

    struct Line
    {
        char     text[TEXT_MAX];
        uint32_t len;
        uint32_t cur;               // edit position
        uint32_t start_x;
        sint32_t start_y;           // above the screen once it scrolled away
        const char* hint;           // the rest of the word suggested (SetHints)
        uint32_t hint_len;          // shown after the line, dimmed; 0: none
    };

    void place(Line* l, uint32_t i)
    {
        uint32_t cols = term::cols();
        uint32_t pos = l->start_x + i;
        sint32_t y = l->start_y + (sint32_t)(pos / cols);
        term::set_cursor(pos % cols, y < 0 ? 0 : (uint32_t)y);
    }

    // Redraw from `from` to the end, then `erase` blanks where the line
    // used to go on; the cursor ends at the edit position.
    void redraw(Line* l, uint32_t from, uint32_t erase)
    {
        place(l, from);
        for (uint32_t i = from; i < l->len; i++)
            term::putc(l->text[i]);
        for (uint32_t i = 0; i < erase; i++)
            term::putc(' ');

        // The terminal wraps right after the last column, scrolling at the
        // bottom row: where the writing ended says how far it scrolled.
        uint32_t cols = term::cols();
        sint32_t end_row = l->start_y + (sint32_t)((l->start_x + l->len + erase) / cols);
        sint32_t bottom = (sint32_t)term::rows() - 1;
        if (end_row > bottom)
            l->start_y -= end_row - bottom;
        place(l, l->cur);
    }

    // The lines the program read before, the newest last, for Up and Down.
    // Kept per process (process::line_history), made at its first ReadLine.
    const uint32_t HISTORY_LINES = 16;

    // SetHints' lists, one after the other: the commands, a NUL, the names,
    // a NUL; each a word a line.
    const uint32_t HINTS_MAX = 8192;

    struct History
    {
        char     text[HISTORY_LINES][TEXT_MAX];
        uint32_t count;
        char     hints[HINTS_MAX];
    };

    // The hint for the word being typed at the end of the line: the rest of
    // the first word of the right list that starts with it (letters in
    // either case), in l->hint and l->hint_len.
    void find_hint(Line* l, const History* h)
    {
        l->hint_len = 0;
        if (!h || l->cur != l->len)
            return;
        uint32_t start = l->len;
        while (start && l->text[start - 1] != ' ')
            start--;
        uint32_t wlen = l->len - start;
        if (!wlen)
            return;
        for (uint32_t i = start; i < l->len; i++)
            if (l->text[i] == '/' || l->text[i] == '"')
                return;

        // A command starts the line, or comes after | or &.
        uint32_t p = start;
        while (p && l->text[p - 1] == ' ')
            p--;
        bool command = p == 0 || l->text[p - 1] == '|' || l->text[p - 1] == '&';
        if (l->text[start] == '&')
        {
            start++;                    // "&prog": the word after it
            wlen--;
            command = true;
            if (!wlen)
                return;
        }
        const char* list = h->hints;
        if (!command)
            list += strlen(list) + 1;

        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; };
        for (const char* w = list; *w;)
        {
            uint32_t n = 0;
            while (w[n] && w[n] != '\n')
                n++;
            bool match = n > wlen;
            for (uint32_t i = 0; match && i < wlen; i++)
                match = lower(w[i]) == lower(l->text[start + i]);
            if (match)
            {
                l->hint = w + wlen;
                l->hint_len = n - wlen;
                return;
            }
            w += n;
            if (*w)
                w++;
        }
    }

    // Draw the hint after the line (`show`), or blank where it was. Cell by
    // cell, so the cursor stays and nothing scrolls: what would go below
    // the screen is left out.
    void draw_hint(const Line* l, bool show)
    {
        uint32_t cols = term::cols(), rows = term::rows();
        for (uint32_t i = 0; i < l->hint_len; i++)
        {
            uint32_t pos = l->start_x + l->len + i;
            sint32_t y = l->start_y + (sint32_t)(pos / cols);
            if (y < 0 || y >= (sint32_t)rows)
                continue;
            term::put_cell(pos % cols, (uint32_t)y, show ? l->hint[i] : ' ',
                           show ? TERM_BLACK + 8 : TERM_DEFAULT_FG, TERM_DEFAULT_BG);
        }
    }

    // Take the hint into the line.
    void take_hint(Line* l)
    {
        uint32_t at = l->len;
        for (uint32_t i = 0; i < l->hint_len && l->len + 1 < TEXT_MAX; i++)
            l->text[l->len++] = l->hint[i];
        l->cur = l->len;
        l->hint_len = 0;
        redraw(l, at, 0);
    }

    void remember(History* h, const Line* l)
    {
        if (!h || l->len == 0)
            return;
        if (h->count && strcmp(h->text[h->count - 1], l->text) == 0)
            return;                     // the same as the last one
        if (h->count == HISTORY_LINES)
        {
            for (uint32_t i = 1; i < HISTORY_LINES; i++)
                memcpy(h->text[i - 1], h->text[i], TEXT_MAX);
            h->count--;
        }
        memcpy(h->text[h->count++], l->text, l->len + 1);
    }

    // Put `text` in place of the line being edited.
    void replace(Line* l, const char* text)
    {
        uint32_t old = l->len;
        l->len = l->cur = (uint32_t)strlen(text);
        memcpy(l->text, text, l->len);
        redraw(l, 0, old > l->len ? old - l->len : 0);
    }

    // Ctrl+V: what the clipboard holds typed in at the edit position, its
    // line breaks as spaces and other control characters left out.
    void paste(Line* l)
    {
        auto typed = [](char c) { return c == '\n' ? ' ' : c; };
        uint32_t n = 0;
        for (uint64_t i = 0; i < clip_len && l->len + n + 1 < TEXT_MAX; i++)
            n += (uint8_t)typed(clip[i]) >= 0x20;
        if (!n)
            return;
        for (uint32_t i = l->len; i > l->cur; i--)
            l->text[i - 1 + n] = l->text[i - 1];
        uint32_t at = l->cur;
        for (uint64_t i = 0; l->cur < at + n; i++)
            if ((uint8_t)typed(clip[i]) >= 0x20)
                l->text[l->cur++] = typed(clip[i]);
        l->len += n;
        redraw(l, at, 0);
    }

    // SF_SUCCESS with the line in l->text, SF_ABORTED or SF_END_OF_FILE.
    SfStatus edit_line(Line* l)
    {
        History* h = (History*)process::line_history(sizeof(History));
        uint32_t back = 0;              // how far back in h the line shown is (0: a new one)
        Selection sel = {};

        {
            OnScreen on;
            l->len = l->cur = 0;
            l->hint_len = 0;
            l->start_x = term::cursor_x();
            l->start_y = (sint32_t)term::cursor_y();
            term::show_cursor();
        }

        for (;;)
        {
            keyboard_event_t e;
            if (!next_key(&e))
            {
                OnScreen on;
                draw_hint(l, false);
                if (sel.on)
                {
                    sel.on = false;
                    show_selection(&sel);
                }
                return SF_ABORTED;
            }
            if (!reportable(e))
                continue;               // Ctrl on its own keeps a selection
            OnScreen on;

            // The hint goes while the key acts, and comes back for the line
            // as the key left it.
            draw_hint(l, false);
            bool had_hint = l->hint_len != 0;
            struct Rehint
            {
                Line* l;
                const History* h;
                bool on;
                ~Rehint() { if (on) { find_hint(l, h); draw_hint(l, true); } }
            } rehint = { l, h, true };

            // Ctrl+arrows select; Ctrl+C copies what is selected, Esc drops
            // it, and any other key drops it and then does what it does.
            if (move_selection(&sel, e))
                continue;
            if (sel.on)
            {
                bool copy = is_ctrl_c(e);
                if (copy)
                    copy_selection(&sel);
                sel.on = false;
                show_selection(&sel);
                if (copy || e.KeyCode == KEY_ESCAPE)
                    continue;
            }
            if (e.Control && e.KeyChar == 0x16)     // Ctrl+V
            {
                paste(l);
                continue;
            }
            if ((e.KeyCode == KEY_TAB || (e.KeyCode == KEY_ARROW_RIGHT && l->cur == l->len)) &&
                had_hint)
            {
                take_hint(l);
                continue;
            }

            if (is_ctrl_c(e))
            {
                rehint.on = false;
                l->cur = l->len;
                redraw(l, l->len, 0);
                tty::write("^C\n", 3);
                return SF_ABORTED;
            }
            if (e.Control && e.KeyChar == 4 && l->len == 0)     // Ctrl+D
            {
                rehint.on = false;
                tty::write("\n", 1);
                return SF_END_OF_FILE;
            }

            switch (e.KeyCode)
            {
                case KEY_ENTER:
                case KEY_KP_ENTER:
                    rehint.on = false;
                    l->cur = l->len;
                    place(l, l->len);
                    tty::write("\n", 1);
                    l->text[l->len] = '\0';
                    remember(h, l);
                    return SF_SUCCESS;
                case KEY_BACKSPACE:
                    if (l->cur == 0)
                        break;
                    for (uint32_t i = l->cur - 1; i + 1 < l->len; i++)
                        l->text[i] = l->text[i + 1];
                    l->cur--;
                    l->len--;
                    redraw(l, l->cur, 1);
                    break;
                case KEY_DELETE:
                    if (l->cur == l->len)
                        break;
                    for (uint32_t i = l->cur; i + 1 < l->len; i++)
                        l->text[i] = l->text[i + 1];
                    l->len--;
                    redraw(l, l->cur, 1);
                    break;
                case KEY_ARROW_LEFT:  if (l->cur > 0) l->cur--;      place(l, l->cur); break;
                case KEY_ARROW_RIGHT: if (l->cur < l->len) l->cur++; place(l, l->cur); break;
                case KEY_HOME:        l->cur = 0;                    place(l, l->cur); break;
                case KEY_END:         l->cur = l->len;               place(l, l->cur); break;
                case KEY_ARROW_UP:
                    if (h && back < h->count)
                        replace(l, h->text[h->count - ++back]);
                    break;
                case KEY_ARROW_DOWN:
                    if (back > 0)
                        replace(l, --back ? h->text[h->count - back] : "");
                    break;
                default:
                    if ((uint8_t)e.KeyChar < 0x20 || l->len + 1 >= TEXT_MAX)
                        break;              // no character, a control one, or full
                    for (uint32_t i = l->len; i > l->cur; i--)
                        l->text[i] = l->text[i - 1];
                    l->text[l->cur] = e.KeyChar;
                    l->len++;
                    uint32_t at = l->cur++;
                    redraw(l, at, 0);
                    break;
            }
        }
    }

    // (const char* Commands, const char* Names)
    void set_hints(user_regs* regs, iret_frame*)
    {
        History* h = (History*)process::line_history(sizeof(History));
        if (!h)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        // Both lists into one buffer; a list cut short loses its tail.
        uint64_t n = 0;
        const uint64_t lists[2] = { regs->rdi, regs->rsi };
        for (uint64_t list : lists)
        {
            sint64_t len = list ? uaccess::strncpy_from_user(h->hints + n, list, HINTS_MAX / 2)
                                : 0;
            if (len == -1)
            {
                h->hints[0] = h->hints[1] = '\0';
                regs->rax = SF_INVALID_PARAMETER;
                return;
            }
            n += len >= 0 ? (uint64_t)len : HINTS_MAX / 2 - 1;
            h->hints[n++] = '\0';
        }
        regs->rax = SF_SUCCESS;
    }

    // (char* Buffer, uint64_t Size, uint64_t* Length)
    void read_line(user_regs* regs, iret_frame*)
    {
        uint64_t buffer = regs->rdi;
        uint64_t size   = regs->rsi;
        uint64_t length = regs->rdx;
        if (!size)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        if (process::console_raw())
        {
            regs->rax = SF_UNSUPPORTED;
            return;
        }

        Line* l = (Line*)kmalloc(sizeof(Line));
        if (!l)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        // SF_START_INPUT: the next line of its file, else one typed.
        uint64_t len = 0;
        int from_file = process::input_line(l->text, size < TEXT_MAX ? size : TEXT_MAX, &len);
        SfStatus st = from_file > 0 ? SF_END_OF_FILE : from_file == 0 ? SF_SUCCESS : edit_line(l);
        if (st == SF_SUCCESS)
        {
            if (from_file < 0)
                len = l->len < size - 1 ? l->len : size - 1;    // a longer line is cut
            l->text[len] = '\0';
            bool ok = uaccess::copy_to_user(buffer, l->text, len + 1) &&
                      (!length || uaccess::copy_to_user(length, &len, sizeof(len)));
            st = ok ? SF_SUCCESS : SF_INVALID_PARAMETER;
        }
        kfree(l);
        regs->rax = st;
    }

    // (uint32_t* Columns, uint32_t* Rows)
    void get_size(user_regs* regs, iret_frame*)
    {
        uint32_t cols = term::cols(), rows = term::rows();
        bool ok = uaccess::copy_to_user(regs->rdi, &cols, sizeof(cols)) &&
                  uaccess::copy_to_user(regs->rsi, &rows, sizeof(rows));
        regs->rax = ok ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    // (uint32_t Column, uint32_t Row, uint8_t Visible)
    void set_cursor(user_regs* regs, iret_frame*)
    {
        if (regs->rdi >= term::cols() || regs->rsi >= term::rows())
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        OnScreen on;
        term::set_cursor((uint32_t)regs->rdi, (uint32_t)regs->rsi);
        if ((uint8_t)regs->rdx)
            term::show_cursor();
        else
            term::hide_cursor();
        regs->rax = SF_SUCCESS;
    }

    // (uint8_t Foreground, uint8_t Background)
    void set_color(user_regs* regs, iret_frame*)
    {
        if (regs->rdi > 15 || regs->rsi > 15)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        OnScreen on;
        term::set_colors((uint8_t)regs->rdi, (uint8_t)regs->rsi);
        regs->rax = SF_SUCCESS;
    }

    // (uint32_t Column, uint32_t Row, const char* Text)
    void write_at(user_regs* regs, iret_frame*)
    {
        if (regs->rdi >= term::cols() || regs->rsi >= term::rows())
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        char* buf = (char*)kmalloc(TEXT_MAX);
        if (!buf)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        // No row is longer than a chunk: the rest would be cut anyway.
        sint64_t len = uaccess::strncpy_from_user(buf, regs->rdx, TEXT_MAX);
        if (len == -1)
        {
            regs->rax = SF_INVALID_PARAMETER;
        }
        else
        {
            OnScreen on;
            term::write_at((uint32_t)regs->rdi, (uint32_t)regs->rsi, buf,
                           len >= 0 ? (uint64_t)len : TEXT_MAX - 1);
            regs->rax = SF_SUCCESS;
        }
        kfree(buf);
    }

    // (uint32_t Column, uint32_t Row, uint32_t Width, uint32_t Height,
    //  const SfCell* Cells)
    void draw(user_regs* regs, iret_frame*)
    {
        uint64_t x = regs->rdi, y = regs->rsi, w = regs->rdx, h = regs->r10;
        uint64_t cells = regs->r8;
        if (!cells || w > 0xFFFF || h > 0xFFFF)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }

        // A row at a time, only what is on the screen.
        uint64_t cols = term::cols(), rows = term::rows();
        uint64_t shown_w = x < cols ? (w < cols - x ? w : cols - x) : 0;
        SfCell* row = shown_w ? (SfCell*)kmalloc(shown_w * sizeof(SfCell)) : nullptr;
        if (shown_w && !row)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        OnScreen on;
        regs->rax = SF_SUCCESS;
        for (uint64_t r = 0; shown_w && r < h && y + r < rows; r++)
        {
            if (!uaccess::copy_from_user(row, cells + r * w * sizeof(SfCell),
                                         shown_w * sizeof(SfCell)))
            {
                regs->rax = SF_INVALID_PARAMETER;
                break;
            }
            for (uint64_t c = 0; c < shown_w; c++)
                term::put_cell((uint32_t)(x + c), (uint32_t)(y + r), row[c].Char,
                               row[c].Color & 0x0F, row[c].Color >> 4);
        }
        if (row)
            kfree(row);
    }

    // (SfKey* Key)
    void read_key(user_regs* regs, iret_frame*)
    {
        keyboard_event_t e;
        for (;;)
        {
            if (!next_key(&e))
            {
                regs->rax = SF_ABORTED;
                return;
            }
            // A modifier on its own is not a key to report.
            if (reportable(e))
                break;
        }
        if (is_ctrl_c(e) && !process::console_raw())
        {
            regs->rax = SF_ABORTED;
            return;
        }

        SfKey key;
        key.Code = e.KeyCode;
        key.Mods = e.Mods & (SF_MOD_SHIFT | SF_MOD_CTRL | SF_MOD_ALT);
        key.Char = e.KeyChar;
        regs->rax = uaccess::copy_to_user(regs->rdi, &key, sizeof(key)) ? SF_SUCCESS
                                                                        : SF_INVALID_PARAMETER;
    }

    // (uint64_t Mode)
    void set_mode(user_regs* regs, iret_frame*)
    {
        if (regs->rdi != SF_CONSOLE_LINE && regs->rdi != SF_CONSOLE_RAW)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        bool raw = regs->rdi == SF_CONSOLE_RAW;
        if (raw != process::console_raw())
        {
            process::set_console_raw(raw);
            OnScreen on;
            term::clear();
            if (raw)
                term::hide_cursor();
            else
                term::show_cursor();
        }
        regs->rax = SF_SUCCESS;
    }

    // ()
    void clear(user_regs* regs, iret_frame*)
    {
        OnScreen on;
        term::clear();
        regs->rax = SF_SUCCESS;
    }

    // () Until the caller has its screen's keys.
    void wait_input(user_regs* regs, iret_frame*)
    {
        regs->rax = process::wait_for_input() ? SF_SUCCESS : SF_ABORTED;
    }

    // (const void* Data, uint64_t Size)
    void set_clipboard(user_regs* regs, iret_frame*)
    {
        uint64_t size = regs->rsi;
        if (size > SF_CLIPBOARD_SIZE)
        {
            regs->rax = SF_BUFFER_TOO_SMALL;
            return;
        }
        char* data = size ? (char*)kmalloc(size) : nullptr;
        if (size && !data)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        if (size && !uaccess::copy_from_user(data, regs->rdi, size))
            regs->rax = SF_INVALID_PARAMETER;
        else
            regs->rax = clip_set(data, size) ? SF_SUCCESS : SF_OUT_OF_RESOURCES;
        if (data)
            kfree(data);
    }

    // (void* Buffer, uint64_t* Size)
    void get_clipboard(user_regs* regs, iret_frame*)
    {
        uint64_t size = 0;
        if (!uaccess::copy_from_user(&size, regs->rsi, sizeof(size)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        // A copy: the clipboard may change while copy_to_user faults in.
        uint64_t len = clip_len;
        char* copy = len <= size && len ? (char*)kmalloc(len) : nullptr;
        if (len <= size && len && !copy)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        if (copy)
            memcpy(copy, clip, len);
        bool ok = uaccess::copy_to_user(regs->rsi, &len, sizeof(len)) &&
                  (!copy || uaccess::copy_to_user(regs->rdi, copy, len));
        if (copy)
            kfree(copy);
        regs->rax = !ok ? SF_INVALID_PARAMETER : len > size ? SF_BUFFER_TOO_SMALL : SF_SUCCESS;
    }

    // (const char* Text)
    void set_title(user_regs* regs, iret_frame*)
    {
        char text[64];
        sint64_t len = uaccess::strncpy_from_user(text, regs->rdi, sizeof(text));
        if (len == -1)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        text[sizeof(text) - 1] = '\0';      // a longer one is cut
        OnScreen on;
        term::set_subtitle(text);
        regs->rax = SF_SUCCESS;
    }
}

namespace sfconsole
{
    bool key_ready()
    {
        uint32_t screen = process::current_screen();
        if (process::input_changed(screen))
            return false;
        // What ReadKey would pass over goes now.
        keyboard_event_t e;
        while (tty::peek_key(&e, screen))
        {
            if (reportable(e))
                return true;
            tty::pop_key(&e, screen);
        }
        return false;
    }

    void init()
    {
        sfcall::set_handler(SFCALL_CONSOLE_PRINT, print);
        sfcall::set_handler(SFCALL_CONSOLE_READLINE, read_line);
        sfcall::set_handler(SFCALL_CONSOLE_GET_SIZE, get_size);
        sfcall::set_handler(SFCALL_CONSOLE_SET_CURSOR, set_cursor);
        sfcall::set_handler(SFCALL_CONSOLE_SET_COLOR, set_color);
        sfcall::set_handler(SFCALL_CONSOLE_WRITE_AT, write_at);
        sfcall::set_handler(SFCALL_CONSOLE_DRAW, draw);
        sfcall::set_handler(SFCALL_CONSOLE_READ_KEY, read_key);
        sfcall::set_handler(SFCALL_CONSOLE_SET_MODE, set_mode);
        sfcall::set_handler(SFCALL_CONSOLE_SET_TITLE, set_title);
        sfcall::set_handler(SFCALL_CONSOLE_CLEAR, clear);
        sfcall::set_handler(SFCALL_CONSOLE_WAIT_INPUT, wait_input);
        sfcall::set_handler(SFCALL_CONSOLE_SET_HINTS, set_hints);
        sfcall::set_handler(SFCALL_CONSOLE_SET_CLIPBOARD, set_clipboard);
        sfcall::set_handler(SFCALL_CONSOLE_GET_CLIPBOARD, get_clipboard);
    }
}
