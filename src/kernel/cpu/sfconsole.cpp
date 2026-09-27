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
        // text goes on.
        OnScreen on;
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
            tty::write(buf, n);
            process::log_output(buf, n);        // in the background: its log
            if (len >= 0)
            {
                regs->rax = SF_SUCCESS;
                break;
            }
            text += TEXT_MAX - 1;
        }
        kfree(buf);
    }

    // The next key press for the caller: false when a signal (the program
    // being ended) cut the wait short.
    bool next_key(keyboard_event_t* e)
    {
        if (!process::wait_for_input())
            return false;
        for (;;)
        {
            uint32_t screen = process::current_screen();
            while (!tty::pop_key(e, screen))
                if (!tty::wait_key(screen))
                    return false;
            if (e->type == KEY_PRESS)
                return true;
        }
    }

    inline bool is_ctrl_c(const keyboard_event_t& e)
    {
        return e.Control && e.KeyChar == 3;
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

    // SF_SUCCESS with the line in l->text, SF_ABORTED or SF_END_OF_FILE.
    SfStatus edit_line(Line* l)
    {
        {
            OnScreen on;
            l->len = l->cur = 0;
            l->start_x = term::cursor_x();
            l->start_y = (sint32_t)term::cursor_y();
            term::show_cursor();
        }

        for (;;)
        {
            keyboard_event_t e;
            if (!next_key(&e))
                return SF_ABORTED;
            OnScreen on;

            if (is_ctrl_c(e))
            {
                l->cur = l->len;
                redraw(l, l->len, 0);
                tty::write("^C\n", 3);
                return SF_ABORTED;
            }
            if (e.Control && e.KeyChar == 4 && l->len == 0)     // Ctrl+D
            {
                tty::write("\n", 1);
                return SF_END_OF_FILE;
            }

            switch (e.KeyCode)
            {
                case KEY_ENTER:
                case KEY_KP_ENTER:
                    l->cur = l->len;
                    place(l, l->len);
                    tty::write("\n", 1);
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

        SfStatus st = edit_line(l);
        if (st == SF_SUCCESS)
        {
            uint64_t len = l->len < size - 1 ? l->len : size - 1;     // a longer line is cut
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
            if (e.KeyChar || (e.KeyCode != KEY_LEFT_SHIFT && e.KeyCode != KEY_RIGHT_SHIFT &&
                              e.KeyCode != KEY_LEFT_CTRL && e.KeyCode != KEY_RIGHT_CTRL &&
                              e.KeyCode != KEY_LEFT_ALT && e.KeyCode != KEY_RIGHT_ALT &&
                              e.KeyCode != KEY_CAPS_LOCK && e.KeyCode != KEY_NUM_LOCK &&
                              e.KeyCode != KEY_SCROLL_LOCK))
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
    }
}
