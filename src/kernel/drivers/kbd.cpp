// The keyboard layer every keyboard driver feeds (see kbd.h).
//
// The state is one for all keyboards: Shift held on one board and a letter
// on another type a capital, and Caps Lock pressed anywhere is Caps Lock
// everywhere - its LED follows on every board.

#include "../../include/drivers/kbd.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/tty.h"
#include "../../include/cpu/process.h"
#include "../../include/drivers/shot.h"

namespace kbd {
    namespace {
        struct state {
            bool lshift, rshift;
            bool lctrl,  rctrl;
            bool lalt,   ralt;          // ralt is AltGr
            bool caps_lock, num_lock, scroll_lock;
        };
        state st = {};

        // Index 1 is Escape: it types 0x1B.
        const char ascii_en[] = {
            0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
            '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
            0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
            0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
            '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };

        const char ascii_en_shift[] = {
            0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
            '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
            0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
            0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
            '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };

        // The keypad block, codes 0x47..0x53. With Num Lock in effect it
        // types; otherwise it navigates. Shift inverts Num Lock, as on a PC.
        struct kp_entry { uint8_t num_code; char num_char; uint8_t nav_code; };
        const kp_entry keypad[] = {
            { KEY_KP_7,      '7', KEY_HOME        },   // 0x47
            { KEY_KP_8,      '8', KEY_ARROW_UP    },   // 0x48
            { KEY_KP_9,      '9', KEY_PAGE_UP     },   // 0x49
            { KEY_KP_MINUS,  '-', KEY_KP_MINUS    },   // 0x4A - always types
            { KEY_KP_4,      '4', KEY_ARROW_LEFT  },   // 0x4B
            { KEY_KP_5,      '5', KEY_KP_5        },   // 0x4C - no navigation
            { KEY_KP_6,      '6', KEY_ARROW_RIGHT },   // 0x4D
            { KEY_KP_PLUS,   '+', KEY_KP_PLUS     },   // 0x4E - always types
            { KEY_KP_1,      '1', KEY_END         },   // 0x4F
            { KEY_KP_2,      '2', KEY_ARROW_DOWN  },   // 0x50
            { KEY_KP_3,      '3', KEY_PAGE_DOWN   },   // 0x51
            { KEY_KP_0,      '0', KEY_INSERT      },   // 0x52
            { KEY_KP_PERIOD, '.', KEY_DELETE      },   // 0x53
        };

        inline bool shift_held() { return st.lshift || st.rshift; }
        inline bool ctrl_held()  { return st.lctrl  || st.rctrl;  }
        inline bool alt_held()   { return st.lalt   || st.ralt;   }

        // Lock state changed: every keyboard's LEDs follow.
        void update_leds() {
            keyboard::set_leds(locks());
        }

        // The kernel's keys; neither the press nor its release reaches anyone:
        //   Alt+F1..F9   show screen 1..9;
        //   Ctrl+Alt+C   end every program on the shown screen, whatever it is
        //                doing - nothing a program does can keep it alive;
        //   Ctrl+Alt+Z   pause them, and pressed again, let them go on;
        //   Print Screen save the panel as a BMP (shot.h).
        const uint8_t KEY_C = 46;
        uint8_t system_held = 0;        // the key whose release to swallow

        bool system_key(uint8_t code, bool pressed) {
            if (code == system_held && !pressed) {
                system_held = 0;
                return true;
            }
            if (code == KEY_PRINT_SCREEN && pressed && !alt_held()) {
                system_held = code;
                shot::request();
                return true;
            }
            if (!pressed || !alt_held())
                return false;
            if (code >= KEY_F1 && code <= KEY_F9) {
                system_held = code;
                term::show((uint32_t)(code - KEY_F1));
                return true;
            }
            if (code == KEY_C && ctrl_held()) {
                system_held = code;
                process::end_screen_programs(term::shown_screen());
                return true;
            }
            if (code == KEY_Z && ctrl_held()) {
                system_held = code;
                process::pause_screen_programs(term::shown_screen());
                return true;
            }
            return false;
        }

        void emit(uint8_t code, char ch, bool pressed) {
            if (system_key(code, pressed))
                return;
            // The key goes to the shown screen's input owner, through that
            // screen's queue (tty.cpp) - nowhere when there is none.
            uint32_t screen = term::shown_screen();
            if (process::screen_input_owner(screen) < 0)
                return;

            keyboard_event_t e = {0};
            e.type    = pressed ? KEY_PRESS : KEY_RELEASE;
            e.KeyCode = code;
            e.KeyChar = ch;
            e.Control = ctrl_held();
            e.Shift   = shift_held();
            e.Alt     = alt_held();
            e.NumLck  = st.num_lock;
            e.ScrLck  = st.scroll_lock;
            e.CapsLck = st.caps_lock;

            uint8_t m = 0;
            if (e.Shift)          m |= KMOD_SHIFT;
            if (e.Control)        m |= KMOD_CTRL;
            if (e.Alt)            m |= KMOD_ALT;
            if (st.ralt)          m |= KMOD_ALTGR;
            if (st.caps_lock)     m |= KMOD_CAPS;
            if (st.num_lock)      m |= KMOD_NUM;
            if (st.scroll_lock)   m |= KMOD_SCROLL;
            e.Mods = m;

            tty::on_key(e, screen);
        }

        char to_ascii(uint8_t code) {
            if (code >= sizeof(ascii_en))
                return 0;

            char c = shift_held() ? ascii_en_shift[code] : ascii_en[code];

            if (c >= 'a' && c <= 'z') {
                if (st.caps_lock)
                    c = c - 'a' + 'A';
            } else if (c >= 'A' && c <= 'Z') {
                if (st.caps_lock && !shift_held())
                    c = c - 'A' + 'a';
            }

            // Ctrl folding: Ctrl+letter types its control character (Ctrl+C is
            // 0x03, Ctrl+D 0x04 - what ReadLine looks for), not the bare letter.
            if (ctrl_held()) {
                if (c >= 'a' && c <= 'z') {
                    c = c - 'a' + 1;            // ^A..^Z -> 0x01..0x1A
                } else if (c >= 'A' && c <= 'Z') {
                    c = c - 'A' + 1;
                } else {
                    switch (c) {
                        case ' ':  c = 0x00; break;     // ^@ (NUL)
                        case '@':  c = 0x00; break;
                        case '[':  c = 0x1B; break;     // ^[ (ESC)
                        case '\\': c = 0x1C; break;
                        case ']':  c = 0x1D; break;
                        case '^':  c = 0x1E; break;
                        case '_':  c = 0x1F; break;
                        case '?':  c = 0x7F; break;     // ^? (DEL)
                        default:   break;               // digits etc: unchanged
                    }
                }
            }

            return c;
        }
    }

    uint8_t locks() {
        uint8_t m = 0;
        if (st.caps_lock)   m |= KMOD_CAPS;
        if (st.num_lock)    m |= KMOD_NUM;
        if (st.scroll_lock) m |= KMOD_SCROLL;
        return m;
    }

    void key(uint8_t code, bool pressed) {
        switch (code) {
            case KEY_LEFT_SHIFT:  st.lshift = pressed; emit(code, 0, pressed); return;
            case KEY_RIGHT_SHIFT: st.rshift = pressed; emit(code, 0, pressed); return;
            case KEY_LEFT_CTRL:   st.lctrl  = pressed; emit(code, 0, pressed); return;
            case KEY_RIGHT_CTRL:  st.rctrl  = pressed; emit(code, 0, pressed); return;
            case KEY_LEFT_ALT:    st.lalt   = pressed; emit(code, 0, pressed); return;
            case KEY_RIGHT_ALT:   st.ralt   = pressed; emit(code, 0, pressed); return;

            case KEY_CAPS_LOCK:
                if (pressed) { st.caps_lock = !st.caps_lock; update_leds(); }
                emit(code, 0, pressed);
                return;
            case KEY_NUM_LOCK:
                if (pressed) { st.num_lock = !st.num_lock; update_leds(); }
                emit(code, 0, pressed);
                return;
            case KEY_SCROLL_LOCK:
                if (pressed) { st.scroll_lock = !st.scroll_lock; update_leds(); }
                emit(code, 0, pressed);
                return;

            case KEY_KP_ENTER: emit(code, '\n', pressed); return;
            case KEY_KP_SLASH: emit(code, '/',  pressed); return;

            default:
                break;
        }

        // Keypad: types or navigates depending on Num Lock, which Shift
        // inverts. 0x37 (KP *) sits outside the block and always types.
        if (code >= 0x47 && code <= 0x53) {
            const kp_entry& k = keypad[code - 0x47];
            bool numeric = st.num_lock ? !shift_held() : shift_held();
            if (numeric || k.nav_code == k.num_code)
                emit(k.num_code, k.num_char, pressed);
            else
                emit(k.nav_code, 0, pressed);
            return;
        }

        // The rest of the 0xE0 keys - the navigation cluster, the Windows
        // and Menu keys, Print Screen, Pause, whatever a board sends - type
        // nothing but are always reported: dropping an unrecognised key is
        // never the right answer.
        if (code & 0x80) {
            emit(code, 0, pressed);
            return;
        }

        emit(code, to_ascii(code), pressed);
    }
}
