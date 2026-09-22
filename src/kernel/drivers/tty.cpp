// Console terminal: termios, the canonical line discipline, and the
// raw-mode key encoder. See tty.h for the contract.

#include "../../include/drivers/tty.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/pit.h"
#include "../../include/mm/memory.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    const uint32_t RING_SIZE   = 128;       // raw events (press + release)
    const uint32_t LINE_MAX    = 1024;      // canonical line buffer
    const uint32_t ENC_MAX     = 32;        // one encoded key, worst case
    const uint32_t RAWQ_SIZE   = 256;       // encoded bytes awaiting a reader

    keyboard_event_t ring[RING_SIZE];
    volatile uint32_t head = 0;
    volatile uint32_t tail = 0;

    // Set by the keyboard IRQ, consumed in syscall context. The flush that
    // ISIG implies happens there too: emptying the line buffer from an
    // interrupt could land in the middle of a read assembling it.
    volatile int  pending_sig = 0;
    volatile bool flush_pending = false;
    volatile bool kill_flag = false;

    pid_t fg = 0;                       // foreground process group

    // Line under construction. Survives read-syscall restarts (the wake
    // model re-executes the syscall; consumed keys are gone from the ring,
    // so the assembly continues exactly where it stopped).
    char     line[LINE_MAX];
    uint32_t line_len_ = 0;         // bytes held
    uint32_t line_cur = 0;          // edit position within them
    bool     line_ready = false;    // terminated by Enter: may be served
    bool     line_eof   = false;    // terminated by the EOF key on an empty line

    // Raw mode: scratch for encoding one key, then a byte queue the reader
    // drains. The queue is the only place encoded bytes live - a short read
    // (fewer than VMIN) simply leaves them in it, so nothing is ever copied
    // out and pushed back.
    uint8_t  enc[ENC_MAX];
    uint32_t enc_len = 0;

    uint8_t  rawq[RAWQ_SIZE];
    uint32_t rq_head = 0;               // write index
    uint32_t rq_tail = 0;               // read index
    // A read found bytes but fewer than VMIN: it is waiting for more input,
    // not for the bytes it already has. Without this, readable() would keep
    // reporting the held bytes and the reader would spin.
    bool     raw_short = false;

    // VTIME deadline for the current raw read, 0 when none is running.
    uint64_t read_deadline = 0;

    struct termios cur;
    bool csi_u_mode = false;

    void default_termios(struct termios* t)
    {
        memory::memset((uint8_t*)t, 0, sizeof(struct termios));
        t->c_iflag = ICRNL | IXON;
        t->c_oflag = OPOST | ONLCR;
        t->c_cflag = CS8 | CREAD;
        t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOCTL | IEXTEN;
        t->c_line = 0;
        t->c_cc[VINTR]  = 3;        // ^C: SIGINT to the foreground group
        t->c_cc[VQUIT]  = 28;       // ^backslash
        t->c_cc[VERASE] = 8;        // backspace
        t->c_cc[VKILL]  = 21;       // ^U
        t->c_cc[VEOF]   = 4;        // ^D
        t->c_cc[VMIN]   = 1;
        t->c_cc[VTIME]  = 0;
        t->c_cc[VSTART] = 17;       // ^Q
        t->c_cc[VSTOP]  = 19;       // ^S
        t->c_cc[VSUSP]  = 26;       // ^Z
        t->c_cc[VWERASE]= 23;       // ^W
        t->c_ispeed = 38400;
        t->c_ospeed = 38400;
    }

    inline bool canonical() { return (cur.c_lflag & ICANON) != 0; }
    inline bool echoing()   { return (cur.c_lflag & ECHO) != 0; }

    void echo(const char* s, uint32_t n)
    {
        if (!echoing())
            return;
        screen::write(s, n);
        // Echo goes to the screen only: the serial log already gets every
        // byte the app writes, typing it twice would garble uart.log.
    }

    // Echo input the way a terminal does: with ECHOCTL a control character
    // shows as ^X rather than being executed. Without it, echoing the bytes
    // an arrow key encodes to would feed a real CSI sequence back to the
    // screen and move the cursor instead of showing anything.
    void echo_input(const uint8_t* b, uint32_t n)
    {
        if (!echoing())
            return;

        bool ctl = (cur.c_lflag & ECHOCTL) != 0;
        for (uint32_t i = 0; i < n; i++)
        {
            uint8_t c = b[i];
            if (ctl && c != '\n' && c != '\t' && c != '\r' &&
                (c < 0x20 || c == 0x7F))
            {
                char out[2] = { '^', (char)(c == 0x7F ? '?' : c + 0x40) };
                screen::write(out, 2);
            }
            else
            {
                screen::write((const char*)&c, 1);
            }
        }
    }

    bool ring_pop(keyboard_event_t* out)
    {
        if (head == tail)
            return false;
        *out = ring[tail];
        tail = (tail + 1) % RING_SIZE;
        return true;
    }

    inline bool ring_empty() { return head == tail; }

    // --- canonical line editing -------------------------------------------

    // Redraw from the edit position to the end, then put the cursor back.
    // The line may have wrapped, so let the terminal do the arithmetic:
    // print the tail, erase one cell, and walk back with CUB.
    void redraw_tail(uint32_t from, uint32_t erase)
    {
        if (!echoing())
            return;
        uint32_t n = line_len_ - from;
        if (n)
            screen::write(line + from, n);
        for (uint32_t i = 0; i < erase; i++)
            screen::write(" ", 1);
        uint32_t back = n + erase;
        for (uint32_t i = 0; i < back; i++)
            screen::write("\033[D", 3);
    }

    void insert_char(char c)
    {
        if (line_len_ >= LINE_MAX - 1)
            return;
        for (uint32_t i = line_len_; i > line_cur; i--)
            line[i] = line[i - 1];
        line[line_cur] = c;
        line_len_++;
        line_cur++;
        echo(&c, 1);
        redraw_tail(line_cur, 0);
    }

    void erase_back()
    {
        if (line_cur == 0)
            return;
        for (uint32_t i = line_cur - 1; i + 1 < line_len_; i++)
            line[i] = line[i + 1];
        line_cur--;
        line_len_--;
        echo("\033[D", 3);
        redraw_tail(line_cur, 1);
    }

    void erase_forward()
    {
        if (line_cur >= line_len_)
            return;
        for (uint32_t i = line_cur; i + 1 < line_len_; i++)
            line[i] = line[i + 1];
        line_len_--;
        redraw_tail(line_cur, 1);
    }

    void kill_line()
    {
        while (line_cur > 0)
            erase_back();
        while (line_len_ > line_cur)
            erase_forward();
    }

    void erase_word()
    {
        while (line_cur > 0 && line[line_cur - 1] == ' ')
            erase_back();
        while (line_cur > 0 && line[line_cur - 1] != ' ')
            erase_back();
    }

    void move_left()
    {
        if (line_cur == 0)
            return;
        line_cur--;
        echo("\033[D", 3);
    }

    void move_right()
    {
        if (line_cur >= line_len_)
            return;
        line_cur++;
        echo("\033[C", 3);
    }

    void move_home() { while (line_cur > 0) move_left(); }
    void move_end()  { while (line_cur < line_len_) move_right(); }

    // --- raw-mode encoding -------------------------------------------------

    inline uint32_t rq_count()
    {
        return (rq_head - rq_tail) & (RAWQ_SIZE - 1);
    }

    inline bool rq_push(uint8_t b)
    {
        uint32_t next = (rq_head + 1) & (RAWQ_SIZE - 1);
        if (next == rq_tail)
            return false;               // full: drop
        rawq[rq_head] = b;
        rq_head = next;
        return true;
    }

    inline uint8_t rq_pop()
    {
        uint8_t b = rawq[rq_tail];
        rq_tail = (rq_tail + 1) & (RAWQ_SIZE - 1);
        return b;
    }

    inline void rq_clear() { rq_head = rq_tail = 0; }

    void emit_byte(uint8_t b)
    {
        if (enc_len < ENC_MAX)
            enc[enc_len++] = b;
    }

    void emit_num(uint32_t v)
    {
        char tmp[10];
        uint32_t n = 0;
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n)
            emit_byte((uint8_t)tmp[--n]);
    }

    // xterm's modifier parameter: 1 + shift(1) + alt(2) + ctrl(4).
    uint32_t mod_param(const keyboard_event_t& e)
    {
        uint32_t m = 1;
        if (e.Shift)   m += 1;
        if (e.Alt)     m += 2;
        if (e.Control) m += 4;
        return m;
    }

    // ESC [ <lead> ; <mods> <final>, dropping the parameters when no
    // modifier is held so plain arrows stay the two-byte classic form.
    void emit_csi(uint32_t lead, char final, const keyboard_event_t& e, bool tilde)
    {
        uint32_t m = mod_param(e);
        emit_byte(0x1B);
        emit_byte('[');
        if (tilde || m != 1)
            emit_num(lead);
        if (m != 1)
        {
            emit_byte(';');
            emit_num(m);
        }
        emit_byte((uint8_t)final);
    }

    // Returns false when the key has no byte representation at all.
    bool encode(const keyboard_event_t& e)
    {
        enc_len = 0;

        switch (e.KeyCode)
        {
            case KEY_ARROW_UP:    emit_csi(1, 'A', e, false); return true;
            case KEY_ARROW_DOWN:  emit_csi(1, 'B', e, false); return true;
            case KEY_ARROW_RIGHT: emit_csi(1, 'C', e, false); return true;
            case KEY_ARROW_LEFT:  emit_csi(1, 'D', e, false); return true;
            case KEY_HOME:        emit_csi(1, 'H', e, false); return true;
            case KEY_END:         emit_csi(1, 'F', e, false); return true;

            case KEY_INSERT:      emit_csi(2, '~', e, true); return true;
            case KEY_DELETE:      emit_csi(3, '~', e, true); return true;
            case KEY_PAGE_UP:     emit_csi(5, '~', e, true); return true;
            case KEY_PAGE_DOWN:   emit_csi(6, '~', e, true); return true;

            // F1-F4 are SS3 in their unmodified form, CSI once a modifier
            // is held - which is what every terminfo entry expects.
            case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
            {
                char f = (char)('P' + (e.KeyCode - KEY_F1));
                if (mod_param(e) == 1)
                {
                    emit_byte(0x1B);
                    emit_byte('O');
                    emit_byte((uint8_t)f);
                }
                else
                {
                    emit_csi(1, f, e, false);
                }
                return true;
            }

            case KEY_F5:  emit_csi(15, '~', e, true); return true;
            case KEY_F6:  emit_csi(17, '~', e, true); return true;
            case KEY_F7:  emit_csi(18, '~', e, true); return true;
            case KEY_F8:  emit_csi(19, '~', e, true); return true;
            case KEY_F9:  emit_csi(20, '~', e, true); return true;
            case KEY_F10: emit_csi(21, '~', e, true); return true;
            case KEY_F11: emit_csi(23, '~', e, true); return true;
            case KEY_F12: emit_csi(24, '~', e, true); return true;

            default:
                break;
        }

        // Enter is CR on the wire. The scancode table gives NL directly,
        // which is what canonical mode wants (ICRNL would have made the
        // same byte), but a raw reader expects what the key really sends.
        if (e.KeyCode == KEY_ENTER || e.KeyCode == KEY_KP_ENTER)
        {
            if (e.Alt)
                emit_byte(0x1B);
            emit_byte('\r');
            return true;
        }

        if (e.KeyChar == 0)
            return false;           // a modifier or a key with no bytes

        if (csi_u_mode)
        {
            // Full fidelity: report the unmodified codepoint and the
            // modifier mask separately, so combinations classic xterm
            // cannot express (Ctrl+digit, Ctrl+Shift+letter, Alt+symbol)
            // survive the trip.
            uint32_t cp = (uint8_t)e.KeyChar;
            if (e.Control && cp < 0x20)
                cp += 0x60;         // ^A (0x01) reports as 'a'
            emit_byte(0x1B);
            emit_byte('[');
            emit_num(cp);
            emit_byte(';');
            emit_num(mod_param(e));
            emit_byte('u');
            return true;
        }

        // Classic: Alt is an ESC prefix, everything else is its byte.
        if (e.Alt)
            emit_byte(0x1B);
        emit_byte((uint8_t)e.KeyChar);
        return true;
    }

    // Drain the event ring into the byte queue, stopping when the queue has
    // no room for another worst-case key.
    void fill_raw()
    {
        while (rq_count() + ENC_MAX < RAWQ_SIZE - 1)
        {
            keyboard_event_t e;
            if (!ring_pop(&e))
                return;
            if (e.type != KEY_PRESS)
                continue;
            if (!encode(e) || !enc_len)
                continue;

            for (uint32_t i = 0; i < enc_len; i++)
                rq_push(enc[i]);
            echo_input(enc, enc_len);
            raw_short = false;          // new input: a short read may proceed
        }
    }
}

namespace tty
{
    void reset()
    {
        head = 0;
        tail = 0;
        pending_sig = 0;
        flush_pending = false;
        kill_flag = false;
        fg = 0;
        line_len_ = 0;
        line_cur = 0;
        line_ready = false;
        line_eof = false;
        enc_len = 0;
        rq_clear();
        raw_short = false;
        read_deadline = 0;
        csi_u_mode = false;
        default_termios(&cur);
    }

    void on_key(keyboard_event_t e)
    {
        // The emergency kill comes first: it has to work when the
        // application has cleared ISIG, caught every signal it can, and is
        // no longer reading anything.
        if (e.type == KEY_PRESS && e.Control && e.Alt &&
            e.KeyCode == KEY_BACKSPACE)
        {
            kill_flag = true;
            return;
        }

        // ISIG keys never reach the application: they become signals for
        // the foreground group. Esc used to end the session here, which
        // made it impossible for an application to see Esc or any escape
        // sequence built on it.
        if (e.type == KEY_PRESS && (cur.c_lflag & ISIG) && e.KeyChar != 0)
        {
            uint8_t ch = (uint8_t)e.KeyChar;
            int s = 0;
            if      (ch == cur.c_cc[VINTR]) s = SIGINT;
            else if (ch == cur.c_cc[VQUIT]) s = SIGQUIT;
            else if (ch == cur.c_cc[VSUSP]) s = SIGTSTP;

            if (s)
            {
                pending_sig = s;
                if (!(cur.c_lflag & NOFLSH))
                    flush_pending = true;
                return;
            }
        }

        uint32_t next = (head + 1) % RING_SIZE;
        if (next == tail)
            return;                 // full: drop

        ring[head] = e;
        head = next;
    }

    bool take_kill()
    {
        if (!kill_flag)
            return false;
        kill_flag = false;
        return true;
    }

    int take_signal()
    {
        int s = pending_sig;
        if (!s)
            return 0;
        pending_sig = 0;

        if (flush_pending)
        {
            flush_pending = false;
            // POSIX: unless NOFLSH, an ISIG key discards everything typed
            // but not yet read.
            tail = head;
            rq_clear();
            raw_short  = false;
            line_len_  = 0;
            line_cur   = 0;
            line_ready = false;
        }
        return s;
    }

    pid_t fg_pgrp()              { return fg; }
    void  set_fg_pgrp(pid_t pgid) { fg = pgid; }

    bool pop_key(keyboard_event_t* out)
    {
        return ring_pop(out);
    }

    bool readable()
    {
        if (canonical())
        {
            // A complete line already, or keys that might finish one.
            return line_ready || line_eof || !ring_empty();
        }
        if (!ring_empty())
            return true;                // new keys: a read can make progress
        if (rq_count() && !raw_short)
            return true;                // queued bytes a read would take
        if (read_deadline && pit::uptime_ms() >= read_deadline)
            return true;
        return false;
    }

    void get_termios(struct termios* t)
    {
        *t = cur;
    }

    void set_termios(const struct termios* t)
    {
        bool was_canonical = canonical();
        cur = *t;

        // Switching discipline mid-line would serve half a line as raw
        // bytes later; drop what was being typed instead.
        if (was_canonical != canonical())
        {
            line_len_ = 0;
            line_cur = 0;
            line_ready = false;
            enc_len = 0;
            rq_clear();
            raw_short = false;
        }
        read_deadline = 0;
    }

    void set_csi_u(bool on) { csi_u_mode = on; }
    bool csi_u()            { return csi_u_mode; }

    // Consume queued events into the canonical line buffer. Returns true
    // when the line is complete (Enter or EOF).
    static bool assemble()
    {
        keyboard_event_t e;
        while (ring_pop(&e))
        {
            if (e.type != KEY_PRESS)
                continue;

            switch (e.KeyCode)
            {
                case KEY_ENTER:
                case KEY_KP_ENTER:
                    move_end();
                    if (line_len_ < LINE_MAX - 1)
                        line[line_len_++] = '\n';
                    line_ready = true;
                    echo("\n", 1);
                    return true;

                case KEY_BACKSPACE:    erase_back();    continue;
                case KEY_DELETE:       erase_forward(); continue;
                case KEY_ARROW_LEFT:   move_left();     continue;
                case KEY_ARROW_RIGHT:  move_right();    continue;
                case KEY_HOME:         move_home();     continue;
                case KEY_END:          move_end();      continue;
                default: break;
            }

            uint8_t ch = (uint8_t)e.KeyChar;

            if (ch && ch == cur.c_cc[VEOF] && e.Control)
            {
                // EOF. On an empty line it ends input for good; after text
                // it flushes what has been typed, without a newline.
                if (line_len_ == 0)
                {
                    line_eof = true;
                    return true;
                }
                line_ready = true;
                return true;
            }
            if (ch && ch == cur.c_cc[VKILL] && e.Control) { kill_line(); continue; }
            if (ch && ch == cur.c_cc[VWERASE] && e.Control) { erase_word(); continue; }
            if (ch && ch == cur.c_cc[VERASE]) { erase_back(); continue; }

            // Anything else non-printable stays out of the line: it used to
            // be inserted verbatim, so Delete put a 0x7F in the buffer.
            if (ch < 0x20 || ch == 0x7F)
                continue;

            insert_char((char)ch);
        }
        return line_ready || line_eof;
    }

    static sint64_t read_canonical(void* dst, uint64_t n)
    {
        if (!line_ready && !line_eof)
            assemble();

        if (line_eof)
        {
            line_eof = false;
            return 0;               // EOF
        }

        if (!line_ready)
            return -EAGAIN;         // caller blocks (Wait::Key) and restarts

        uint64_t count = line_len_;
        if (count > n)
            count = n;

        memory::memcpy((uint8_t*)dst, (uint8_t*)line, count);

        if (count == (uint64_t)line_len_)
        {
            line_len_ = 0;
            line_cur = 0;
            line_ready = false;
        }
        else
        {
            uint32_t rem = line_len_ - (uint32_t)count;
            for (uint32_t i = 0; i < rem; i++)
                line[i] = line[count + i];
            line_len_ = rem;
            line_cur = 0;
        }
        return (sint64_t)count;
    }

    // Hand over `want` queued bytes. Nothing leaves the queue on any path
    // that returns -EAGAIN, so a restarted syscall sees exactly what this
    // one saw.
    static sint64_t drain(void* dst, uint64_t want)
    {
        uint8_t* out = (uint8_t*)dst;
        for (uint64_t i = 0; i < want; i++)
            out[i] = rq_pop();

        read_deadline = 0;
        raw_short = false;
        return (sint64_t)want;
    }

    static sint64_t read_raw(void* dst, uint64_t n)
    {
        fill_raw();

        uint64_t avail = rq_count();
        uint64_t want  = avail < n ? avail : n;

        uint8_t vmin  = cur.c_cc[VMIN];
        uint8_t vtime = cur.c_cc[VTIME];

        // Linux returns as soon as min(n, VMIN) bytes are there: asking for
        // fewer bytes than VMIN must not block forever.
        uint64_t need = vmin < n ? (uint64_t)vmin : n;

        if (vmin == 0)
        {
            // VMIN 0 with VTIME 0 is a pure poll: report what there is,
            // even nothing. With VTIME it waits for the first byte only.
            if (vtime == 0 || want > 0)
                return drain(dst, want);
        }
        else if (want > 0 && avail >= need)
        {
            return drain(dst, want);
        }

        if (vtime)
        {
            // VTIME is in tenths of a second, measured from the first read
            // that found nothing. The deadline lives here rather than in
            // the caller because the syscall is restarted from scratch on
            // every wake, so it has nowhere of its own to keep it.
            uint64_t now = pit::uptime_ms();
            if (read_deadline == 0)
                read_deadline = now + (uint64_t)vtime * 100;
            else if (now >= read_deadline)
                return drain(dst, want);    // may legitimately be 0
        }

        raw_short = (want > 0);
        return -EAGAIN;
    }

    sint64_t read(void* dst, uint64_t n)
    {
        if (n == 0)
            return 0;
        return canonical() ? read_canonical(dst, n) : read_raw(dst, n);
    }

    uint64_t write(const void* src, uint64_t n)
    {
        screen::write((const char*)src, n);
        uart::write((const char*)src, n);
        return n;
    }

    uint32_t line_len()
    {
        return line_len_;
    }
}
