// Console terminal (stage 3.5): keyboard ring, canonical line discipline,
// echo. See tty.h for the contract and why blocking lives in process.cpp.

#include "../../include/drivers/tty.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/uart.h"
#include "../../include/mm/memory.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    const uint32_t RING_SIZE   = 128;       // raw events (press + release)
    const uint32_t LINE_MAX    = 1024;      // canonical line buffer

    keyboard_event_t ring[RING_SIZE];
    volatile uint32_t head = 0;
    volatile uint32_t tail = 0;
    volatile bool esc_flag = false;

    // Line under construction. Survives read-syscall restarts (the wake
    // model re-executes the syscall; consumed keys are gone from the ring,
    // so the assembly continues exactly where it stopped).
    char    line[LINE_MAX];
    uint32_t line_pos = 0;
    bool    line_ready = false;     // terminated by Enter: may be served
    bool    line_eof   = false;     // terminated by Ctrl+D on an empty line

    void echo(const char* s, uint32_t n)
    {
        screen::write(s, n);
        // Echo goes to the screen only: the serial log already gets every
        // byte the app writes, typing it twice would garble uart.log.
    }
}

namespace tty
{
    void reset()
    {
        head = 0;
        tail = 0;
        esc_flag = false;
        line_pos = 0;
        line_ready = false;
        line_eof = false;
    }

    void on_key(keyboard_event_t e)
    {
        // Esc belongs to the kernel while a session runs: it terminates the
        // session (acted on at the next ring-3 boundary, see process.cpp).
        if (e.KeyCode == KEY_ESCAPE)
        {
            if (e.type == KEY_PRESS)
                esc_flag = true;
            return;
        }

        uint32_t next = (head + 1) % RING_SIZE;
        if (next == tail)
            return;                 // full: drop

        ring[head] = e;
        head = next;
    }

    bool esc_pressed()
    {
        return esc_flag;
    }

    bool has_input()
    {
        return head != tail;
    }

    bool pop_key(keyboard_event_t* out)
    {
        if (head == tail)
            return false;
        *out = ring[tail];
        tail = (tail + 1) % RING_SIZE;
        return true;
    }

    // Consume queued events into the canonical line buffer. Returns true
    // when the line is complete (Enter or Ctrl+D).
    bool assemble()
    {
        keyboard_event_t e;
        while (pop_key(&e))
        {
            if (e.type != KEY_PRESS)
                continue;

            if (e.KeyCode == KEY_ENTER)
            {
                if (line_pos < LINE_MAX - 1)
                    line[line_pos++] = '\n';
                line_ready = true;
                echo("\n", 1);
                return true;
            }

            if (e.KeyCode == KEY_BACKSPACE)
            {
                if (line_pos > 0)
                {
                    line_pos--;
                    echo("\b \b", 3);
                }
                continue;
            }

            // Ctrl+D (0x04): EOF. On an empty line it ends input for good;
            // after text it flushes the line without a newline.
            if (e.Control && e.KeyChar == 0x04)
            {
                if (line_pos == 0)
                {
                    line_eof = true;
                    return true;
                }
                line_ready = true;
                return true;
            }

            // Ignore other control characters (Ctrl+C has no signal to
            // deliver until stage 4).
            if (e.Control)
                continue;

            if (e.KeyChar && line_pos < LINE_MAX - 1)
            {
                line[line_pos++] = e.KeyChar;
                echo(&e.KeyChar, 1);
            }
        }
        return line_ready || line_eof;
    }

    sint64_t read(void* dst, uint64_t n)
    {
        if (n == 0)
            return 0;

        // Serve a previously completed line first (a short read leaves the
        // rest for the next call - POSIX canonical behaviour).
        if (!line_ready && !line_eof)
        {
            assemble();
        }

        if (line_eof)
        {
            line_eof = false;
            return 0;               // EOF
        }

        if (!line_ready)
            return -EAGAIN;         // caller blocks (Wait::Key) and restarts

        uint64_t count = line_pos;
        if (count > n)
            count = n;

        memory::memcpy((uint8_t*)dst, (uint8_t*)line, count);

        if (count == (uint64_t)line_pos)
        {
            // Line fully served: reset the discipline.
            line_pos = 0;
            line_ready = false;
        }
        else
        {
            // Partial: shift the remainder down.
            uint32_t rem = line_pos - (uint32_t)count;
            for (uint32_t i = 0; i < rem; i++)
                line[i] = line[count + i];
            line_pos = rem;
        }
        return (sint64_t)count;
    }

    uint64_t write(const void* src, uint64_t n)
    {
        screen::write((const char*)src, n);
        uart::write((const char*)src, n);
        return n;
    }

    void fill_termios(struct termios* t)
    {
        memory::memset((uint8_t*)t, 0, sizeof(struct termios));
        t->c_iflag = ICRNL | IXON;
        t->c_oflag = OPOST | ONLCR;
        t->c_cflag = CS8 | CREAD;
        t->c_lflag = ISIG | ICANON | ECHO | ECHOE | IEXTEN;
        t->c_line = 0;
        t->c_cc[VINTR]  = 3;        // ^C (no signals yet)
        t->c_cc[VQUIT]  = 28;       // ^\
        t->c_cc[VERASE] = 8;        // backspace
        t->c_cc[VKILL]  = 21;       // ^U
        t->c_cc[VEOF]   = 4;        // ^D
        t->c_cc[VMIN]   = 1;
        t->c_cc[VTIME]  = 0;
        t->c_cc[VSTART] = 17;       // ^Q
        t->c_cc[VSTOP]  = 19;       // ^S
        t->c_cc[VSUSP]  = 26;       // ^Z
        t->c_ispeed = 38400;
        t->c_ospeed = 38400;
    }

    uint32_t line_len()
    {
        return line_pos;
    }
}
