// PS/2 keyboard, scancode set 1.
//
// Every key on a standard 104-key board produces an event. What used to be
// a whitelist of the handful of extended scancodes anyone had needed is now
// a full decode: an application that cannot see a key has no way to work
// around it. Codes are the scancode itself, or `scancode | 0x80` for the
// 0xE0-prefixed keys; abi/keyboard.h names them all.
//
// Two keys are not a single scancode and are handled explicitly:
//   Print Screen  E0 2A E0 37  (press)   E0 B7 E0 AA (release)
//   Pause         E1 1D 45 E1 9D C5      (press only, no release)
// The 0x2A/0xAA halves are a "fake shift" the controller inserts so that
// DOS-era software saw a consistent shift state; they are dropped here.

#include "../../include/drivers/keyboard.h"

namespace keyboard {
    uint8_t scancode_to_ascii(uint8_t scancode);

    static keyboard_state_t kb_state  = {0};

    static const char scancode_to_ascii_en[] = {
        0,  0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
        '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    static const char scancode_to_ascii_en_shift[] = {
        0,  0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
        '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    // The keypad block, scancodes 0x47..0x53. With Num Lock in effect it
    // types; otherwise it navigates. Shift inverts Num Lock, as on a PC.
    struct kp_entry { uint8_t num_code; char num_char; uint8_t nav_code; };
    static const kp_entry keypad[] = {
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

    static keyboard_callback_t user_callback = nullptr;

    void set_keyboard_callback(keyboard_callback_t callback) {
        user_callback = callback;
    }

    void del_keyboard_callback(void) {
        user_callback = nullptr;
    }

    static void send_command(uint8_t command) {
        int attempts = 1000;
        while ((port::byte_in(KEYBOARD_STATUS_PORT) & 0x02) && --attempts > 0);
        if (attempts == 0) return;

        port::byte_out(KEYBOARD_COMMAND_PORT, command);
    }

    static void send_data(uint8_t data) {
        int attempts = 1000;
        while ((port::byte_in(KEYBOARD_STATUS_PORT) & 0x02) && --attempts > 0);     // in
        if (attempts == 0) return;

        port::byte_out(KEYBOARD_DATA_PORT, data);
    }

    static uint8_t read_data(void) {
        int attempts = 1000;
        while (!(port::byte_in(KEYBOARD_STATUS_PORT) & 0x01) && --attempts > 0);  // out
        if (attempts == 0) return 0;

        return port::byte_in(KEYBOARD_DATA_PORT);
    }

    static void set_leds(void) {
        uint8_t led_state = 0;

        if (kb_state.caps_lock)     led_state |= 0x04;
        if (kb_state.num_lock)      led_state |= 0x02;
        if (kb_state.scroll_lock)   led_state |= 0x01;

        send_data(0xED);
        read_data();
        send_data(led_state);
        read_data();
    }

    void init(void) {
        // Reset keyboard controller
        send_command(0xAE);         // Disable keyboard
        send_command(0x20);         // Read configuration
        uint8_t config = read_data();
        send_command(0x60);         // Write configuration
        send_data(config | 0x01);   // Enable interrupts
        send_command(0xAF);         // Enable keyboard

        // Clear buffer
        while (port::byte_in(KEYBOARD_STATUS_PORT) & 0x01) {
            port::byte_in(KEYBOARD_DATA_PORT);
        }

        for (uint32_t i = 0; i < sizeof(kb_state); i++)
            ((uint8_t*)&kb_state)[i] = 0;
        set_leds();
    }

    keyboard_callback_t get_callback() {
        return user_callback;
    }

    static inline bool shift_held() {
        return kb_state.lshift_pressed || kb_state.rshift_pressed;
    }
    static inline bool ctrl_held() {
        return kb_state.lctrl_pressed || kb_state.rctrl_pressed;
    }
    static inline bool alt_held() {
        return kb_state.lalt_pressed || kb_state.ralt_pressed;
    }

    static void emit(uint8_t code, char ch, bool pressed) {
        if (user_callback == nullptr)
            return;

        keyboard_event_t e = {0};
        e.type    = pressed ? KEY_PRESS : KEY_RELEASE;
        e.KeyCode = code;
        e.KeyChar = ch;
        e.Control = ctrl_held();
        e.Shift   = shift_held();
        e.Alt     = alt_held();
        e.NumLck  = kb_state.num_lock;
        e.ScrLck  = kb_state.scroll_lock;
        e.CapsLck = kb_state.caps_lock;

        uint8_t m = 0;
        if (e.Shift)                 m |= KMOD_SHIFT;
        if (e.Control)               m |= KMOD_CTRL;
        if (e.Alt)                   m |= KMOD_ALT;
        if (kb_state.ralt_pressed)   m |= KMOD_ALTGR;
        if (kb_state.caps_lock)      m |= KMOD_CAPS;
        if (kb_state.num_lock)       m |= KMOD_NUM;
        if (kb_state.scroll_lock)    m |= KMOD_SCROLL;
        e.Mods = m;

        user_callback(e);
    }

    // A key behind the 0xE0 prefix.
    static void handle_extended(uint8_t sc, bool pressed) {
        switch (sc) {
            // The controller brackets Print Screen with a fake shift so that
            // software watching the shift state saw it unchanged. It is not
            // a key, and reporting it would look like a real shift press.
            case 0x2A:
            case 0x36:
                return;

            case RCTRL_SCANCODE:
                kb_state.rctrl_pressed = pressed;
                emit(KEY_RIGHT_CTRL, 0, pressed);
                return;
            case RALT_SCANCODE:
                kb_state.ralt_pressed = pressed;
                emit(KEY_RIGHT_ALT, 0, pressed);
                return;

            case 0x1C: emit(KEY_KP_ENTER, '\n', pressed); return;
            case 0x35: emit(KEY_KP_SLASH, '/',  pressed); return;
            case 0x37: emit(KEY_PRINT_SCREEN, 0, pressed); return;

            default:
                // Everything else keeps its scancode, marked extended: the
                // navigation cluster, the Windows and Menu keys, and
                // whatever else a particular board sends. Dropping an
                // unrecognised key is never the right answer.
                emit((uint8_t)(sc | 0x80), 0, pressed);
                return;
        }
    }

    static void handle_plain(uint8_t sc, bool pressed) {
        switch (sc) {
            case LSHIFT_SCANCODE:
                kb_state.lshift_pressed = pressed;
                emit(KEY_LEFT_SHIFT, 0, pressed);
                return;
            case RSHIFT_SCANCODE:
                kb_state.rshift_pressed = pressed;
                emit(KEY_RIGHT_SHIFT, 0, pressed);
                return;
            case LCTRL_SCANCODE:
                kb_state.lctrl_pressed = pressed;
                emit(KEY_LEFT_CTRL, 0, pressed);
                return;
            case LALT_SCANCODE:
                kb_state.lalt_pressed = pressed;
                emit(KEY_LEFT_ALT, 0, pressed);
                return;

            case CAPS_LOCK_SCANCODE:
                if (pressed) { kb_state.caps_lock = !kb_state.caps_lock; set_leds(); }
                emit(KEY_CAPS_LOCK, 0, pressed);
                return;
            case NUM_LOCK_SCANCODE:
                if (pressed) { kb_state.num_lock = !kb_state.num_lock; set_leds(); }
                emit(KEY_NUM_LOCK, 0, pressed);
                return;
            case SCROLL_LOCK_SCANCODE:
                if (pressed) { kb_state.scroll_lock = !kb_state.scroll_lock; set_leds(); }
                emit(KEY_SCROLL_LOCK, 0, pressed);
                return;

            default:
                break;
        }

        // Keypad: types or navigates depending on Num Lock, which Shift
        // inverts. 0x37 (KP *) sits outside the block and always types.
        if (sc >= 0x47 && sc <= 0x53) {
            const kp_entry& k = keypad[sc - 0x47];
            bool numeric = kb_state.num_lock ? !shift_held() : shift_held();
            if (numeric || k.nav_code == k.num_code)
                emit(k.num_code, k.num_char, pressed);
            else
                emit(k.nav_code, 0, pressed);
            return;
        }

        emit(sc, (char)scancode_to_ascii(sc), pressed);
    }

    void handler(void) {
        uint8_t scancode = port::byte_in(KEYBOARD_DATA_PORT);

        // Tail of a multi-byte sequence whose meaning was already reported.
        if (kb_state.skip_bytes) {
            kb_state.skip_bytes--;
            return;
        }

        if (scancode == PAUSE_PREFIX) {
            // E1 1D 45 E1 9D C5 arrives in one go and no release ever
            // follows. Report the press now and swallow the remaining five
            // bytes, which would otherwise decode as Ctrl and Num Lock.
            kb_state.skip_bytes = 5;
            emit(KEY_PAUSE, 0, true);
            return;
        }

        if (scancode == EXTENDED_SCANCODE) {
            kb_state.extended_code = 1;
            return;
        }

        uint8_t pressed = !(scancode & KEY_RELEASED_MASK);
        scancode &= ~KEY_RELEASED_MASK;

        if (kb_state.extended_code) {
            kb_state.extended_code = 0;
            handle_extended(scancode, pressed);
            return;
        }

        handle_plain(scancode, pressed);
    }

    uint8_t scancode_to_ascii(uint8_t scancode) {
        if (scancode >= sizeof(scancode_to_ascii_en)) {
            return 0;
        }

        char c;
        if (shift_held()) {
            c = scancode_to_ascii_en_shift[scancode];
        } else {
            c = scancode_to_ascii_en[scancode];
        }

        if (c >= 'a' && c <= 'z') {
            if (kb_state.caps_lock) {
                c = c - 'a' + 'A';
            }
        } else if (c >= 'A' && c <= 'Z') {
            if (kb_state.caps_lock && !shift_held()) {
                c = c - 'A' + 'a';
            }
        }

        // Ctrl folding. Without this Ctrl+D delivered 'd', so the EOF check
        // in tty::assemble (KeyChar == 0x04) could never fire, and
        // Ctrl+letter was inserted into the line as the bare letter.
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
} // namespace
