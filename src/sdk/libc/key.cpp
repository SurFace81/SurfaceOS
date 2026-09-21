// Turn the terminal's byte stream back into keys. The mirror image of the
// encoder in src/kernel/drivers/tty.cpp - keep the two in step.

#include "../include/key.h"
#include "../include/termios.h"
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdio.h"

namespace
{
    // Bytes read ahead of what the caller asked for. A sequence arrives in
    // one read, but a read can also return several keys at once.
    uint8_t  buf[64];
    uint32_t buf_len = 0;
    uint32_t buf_pos = 0;

    bool raw_active = false;
    struct termios saved;
    bool saved_valid = false;

    // -1 when nothing arrived before the tty's VTIME expired.
    int next_byte()
    {
        if (buf_pos < buf_len)
            return buf[buf_pos++];

        ssize_t n = read(0, buf, sizeof(buf));
        if (n <= 0)
        {
            buf_len = buf_pos = 0;
            return -1;
        }
        buf_len = (uint32_t)n;
        buf_pos = 0;
        return buf[buf_pos++];
    }

    // Blocking version: keeps waiting through the VTIME timeouts.
    int next_byte_blocking()
    {
        for (;;)
        {
            int c = next_byte();
            if (c >= 0)
                return c;
        }
    }

    void unget(int c)
    {
        if (c >= 0 && buf_pos > 0)
            buf[--buf_pos] = (uint8_t)c;
    }

    // xterm's modifier parameter is 1 + shift(1) + alt(2) + ctrl(4).
    uint8_t mods_from_param(uint32_t p)
    {
        if (p == 0)
            return 0;
        p -= 1;
        uint8_t m = 0;
        if (p & 1) m |= KEY_MOD_SHIFT;
        if (p & 2) m |= KEY_MOD_ALT;
        if (p & 4) m |= KEY_MOD_CTRL;
        return m;
    }

    uint32_t tilde_code(uint32_t n)
    {
        switch (n)
        {
            case 2:  return KEY_INSERT;
            case 3:  return KEY_DELETE;
            case 5:  return KEY_PAGE_UP;
            case 6:  return KEY_PAGE_DOWN;
            case 15: return KEY_F5;
            case 17: return KEY_F6;
            case 18: return KEY_F7;
            case 19: return KEY_F8;
            case 20: return KEY_F9;
            case 21: return KEY_F10;
            case 23: return KEY_F11;
            case 24: return KEY_F12;
            default: return 0;
        }
    }

    // ESC [ ... already consumed.
    bool decode_csi(struct key_event* out)
    {
        uint32_t params[4] = { 0, 0, 0, 0 };
        uint32_t np = 0;
        bool seen = false;

        for (;;)
        {
            int c = next_byte_blocking();
            if (c >= '0' && c <= '9')
            {
                if (np == 0) np = 1;
                if (np <= 4) params[np - 1] = params[np - 1] * 10 + (uint32_t)(c - '0');
                seen = true;
                continue;
            }
            if (c == ';')
            {
                if (np == 0) np = 1;
                if (np < 4) np++;
                seen = true;
                continue;
            }
            if (c < 0x40 || c > 0x7E)
                continue;               // intermediates and stray bytes

            uint8_t m = mods_from_param(np > 1 ? params[1] : 0);

            switch (c)
            {
                case 'A': out->code = KEY_ARROW_UP;    out->mods = m; return true;
                case 'B': out->code = KEY_ARROW_DOWN;  out->mods = m; return true;
                case 'C': out->code = KEY_ARROW_RIGHT; out->mods = m; return true;
                case 'D': out->code = KEY_ARROW_LEFT;  out->mods = m; return true;
                case 'H': out->code = KEY_HOME;        out->mods = m; return true;
                case 'F': out->code = KEY_END;         out->mods = m; return true;

                case 'P': out->code = KEY_F1; out->mods = m; return true;
                case 'Q': out->code = KEY_F2; out->mods = m; return true;
                case 'R': out->code = KEY_F3; out->mods = m; return true;
                case 'S': out->code = KEY_F4; out->mods = m; return true;

                case '~':
                {
                    uint32_t k = tilde_code(seen ? params[0] : 0);
                    if (!k)
                        return false;   // unknown: skip the whole sequence
                    out->code = k;
                    out->mods = m;
                    return true;
                }

                case 'u':
                    // Full-fidelity form: codepoint ; modifiers u. This is
                    // the only way Ctrl+digit or Ctrl+Shift+letter survive.
                    // Escape arrives here too, as codepoint 27; hand it back
                    // under the name callers match on.
                    out->code = (params[0] == 0x1B) ? (uint32_t)KEY_ESCAPE
                                                    : params[0];
                    out->mods = mods_from_param(np > 1 ? params[1] : 0);
                    return true;

                default:
                    return false;
            }
        }
    }
}

extern "C" {

int key_raw_mode(int on)
{
    struct termios t;

    if (on)
    {
        if (!saved_valid)
        {
            if (tcgetattr(0, &saved) != 0)
                return -1;
            saved_valid = true;
        }
        t = saved;
        cfmakeraw(&t);
        // VMIN 0 with VTIME 1 gives a 100 ms read timeout, which is what
        // lets a lone Escape be told from the start of a sequence.
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 1;
        if (tcsetattr(0, TCSANOW, &t) != 0)
            return -1;
        print("\033[>1u");          // ask for full-fidelity keys
        raw_active = true;
        return 0;
    }

    print("\033[<u");
    raw_active = false;
    if (saved_valid)
        return tcsetattr(0, TCSANOW, &saved);
    return 0;
}

int key_read(struct key_event* out)
{
    if (!out)
        return -1;

    out->code = 0;
    out->mods = 0;

    int c = next_byte_blocking();
    if (c < 0)
        return -1;

    if (c != 0x1B)
    {
        // Control codes come through as themselves; report them the way the
        // key was actually struck.
        if (c >= 1 && c <= 26 && c != '\r' && c != '\n' && c != '\t' && c != '\b')
        {
            out->code = (uint32_t)('a' + c - 1);
            out->mods = KEY_MOD_CTRL;
            return 0;
        }
        out->code = (uint32_t)c;
        return 0;
    }

    // ESC. Either a sequence, or the Escape key on its own - which is only
    // knowable once the tty's read timeout expires with nothing following.
    int n = next_byte();
    if (n < 0)
    {
        out->code = KEY_ESCAPE;
        return 0;
    }

    if (n == '[')
    {
        if (decode_csi(out))
            return 0;
        // An unrecognised sequence: report nothing rather than garbage.
        return key_read(out);
    }

    if (n == 'O')
    {
        int f = next_byte_blocking();
        switch (f)
        {
            case 'P': out->code = KEY_F1; return 0;
            case 'Q': out->code = KEY_F2; return 0;
            case 'R': out->code = KEY_F3; return 0;
            case 'S': out->code = KEY_F4; return 0;
            default:  return key_read(out);
        }
    }

    // ESC followed by an ordinary byte is Alt+that key.
    if (n >= 1 && n <= 26)
    {
        out->code = (uint32_t)('a' + n - 1);
        out->mods = KEY_MOD_ALT | KEY_MOD_CTRL;
        return 0;
    }
    out->code = (uint32_t)n;
    out->mods = KEY_MOD_ALT;
    return 0;
}

char* key_name(const struct key_event* e, char* buf_out, uint32_t n)
{
    if (!buf_out || n == 0)
        return buf_out;
    buf_out[0] = '\0';

    if (e->mods & KEY_MOD_CTRL)  strcat(buf_out, "Ctrl+");
    if (e->mods & KEY_MOD_ALT)   strcat(buf_out, "Alt+");
    if (e->mods & KEY_MOD_SHIFT) strcat(buf_out, "Shift+");

    const char* name = 0;
    switch (e->code)
    {
        case KEY_ESCAPE:      name = "Esc";      break;
        case KEY_ARROW_UP:    name = "Up";       break;
        case KEY_ARROW_DOWN:  name = "Down";     break;
        case KEY_ARROW_LEFT:  name = "Left";     break;
        case KEY_ARROW_RIGHT: name = "Right";    break;
        case KEY_HOME:        name = "Home";     break;
        case KEY_END:         name = "End";      break;
        case KEY_INSERT:      name = "Insert";   break;
        case KEY_DELETE:      name = "Delete";   break;
        case KEY_PAGE_UP:     name = "PageUp";   break;
        case KEY_PAGE_DOWN:   name = "PageDown"; break;
        case KEY_F1:  name = "F1";  break;
        case KEY_F2:  name = "F2";  break;
        case KEY_F3:  name = "F3";  break;
        case KEY_F4:  name = "F4";  break;
        case KEY_F5:  name = "F5";  break;
        case KEY_F6:  name = "F6";  break;
        case KEY_F7:  name = "F7";  break;
        case KEY_F8:  name = "F8";  break;
        case KEY_F9:  name = "F9";  break;
        case KEY_F10: name = "F10"; break;
        case KEY_F11: name = "F11"; break;
        case KEY_F12: name = "F12"; break;
        case '\r': case '\n': name = "Enter";     break;
        case '\t':            name = "Tab";       break;
        case 0x7F: case '\b': name = "Backspace"; break;
        case ' ':             name = "Space";     break;
        default: break;
    }

    if (name)
    {
        strcat(buf_out, name);
    }
    else if (e->code < 0x80 && e->code >= 0x20)
    {
        uint32_t l = strlen(buf_out);
        if (l + 2 <= n)
        {
            buf_out[l] = (char)e->code;
            buf_out[l + 1] = '\0';
        }
    }
    else
    {
        strcat(buf_out, "?");
    }
    return buf_out;
}

}
