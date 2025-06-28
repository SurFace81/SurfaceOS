#include "../../include/drivers/keyboard.h"

namespace keyboard {
    UINT8 scancode_to_ascii(UINT8 scancode);
    UINT8 extended_scancode_to_ascii(UINT8 scancode);

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

    static keyboard_callback_t user_callback = nullptr;

    void set_keyboard_callback(keyboard_callback_t callback) {
        user_callback = callback;
    }

    void del_keyboard_callback(void) {
        user_callback = nullptr;
    }

    static void send_command(UINT8 command) {
        int attempts = 1000;
        while ((port::byte_in(KEYBOARD_STATUS_PORT) & 0x02) && --attempts > 0);
        if (attempts == 0) return;

        port::byte_out(KEYBOARD_COMMAND_PORT, command);
    }

    static void send_data(UINT8 data) {
        int attempts = 1000;
        while ((port::byte_in(KEYBOARD_STATUS_PORT) & 0x02) && --attempts > 0);     // in
        if (attempts == 0) return;

        port::byte_out(KEYBOARD_DATA_PORT, data);
    }

    static UINT8 read_data(void) {
        int attempts = 1000;
        while (!(port::byte_in(KEYBOARD_STATUS_PORT) & 0x01) && --attempts > 0);  // out
        if (attempts == 0) return 0;

        return port::byte_in(KEYBOARD_DATA_PORT);
    }

    static void set_leds(void) {
        UINT8 led_state = 0;
        
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
        UINT8 config = read_data();
        send_command(0x60);         // Write configuration  
        send_data(config | 0x01);   // Enable interrupts
        send_command(0xAF);         // Enable keyboard
        
        // Clear buffer
        while (port::byte_in(KEYBOARD_STATUS_PORT) & 0x01) {
            port::byte_in(KEYBOARD_DATA_PORT);
        }

        kb_state.caps_lock      = 0;
        kb_state.num_lock       = 0;
        kb_state.scroll_lock    = 0;
        kb_state.alt_pressed    = 0;
        kb_state.shift_pressed  = 0;
        kb_state.ctrl_pressed   = 0;
        set_leds();
    }

    void handler(void) {
        UINT8 scancode = port::byte_in(KEYBOARD_DATA_PORT);
        
        if (scancode == EXTENDED_SCANCODE) {
            kb_state.extended_code = 1;
            return;
        }
        
        UINT8 pressed = !(scancode & KEY_RELEASED_MASK);
        scancode &= ~KEY_RELEASED_MASK;
        
        if (kb_state.extended_code) {
            kb_state.extended_code = 0;
            
            switch (scancode) {
                case 0x1D:
                    kb_state.ctrl_pressed = pressed;
                    break;
                case 0x38:
                    kb_state.alt_pressed = pressed;
                    break;
                case 0x48: // Up Arrow
                case 0x50: // Down Arrow  
                case 0x4B: // Left Arrow
                case 0x4D: // Right Arrow
                case 0x49: // Page Up
                case 0x51: // Page Down
                case 0x47: // Home
                case 0x4F: // End
                case 0x52: // Insert
                case 0x53: // Delete
                case 0x1C: // NumPad Enter
                case 0x35: // NumPad /
                case 0x5B: // Left Windows Key
                case 0x5C: // Right Windows Key
                case 0x5D: // Menu Key
                case 0x46: // Ctrl+Break
                case 0x45: // Num Lock (extended version)
                    if (user_callback != nullptr) {
                        keyboard_event_t e = {0};
                        e.type    = pressed ? KEY_PRESS : KEY_RELEASE;
                        e.KeyCode = scancode | 0x80; // Mark as extended
                        e.KeyChar = extended_scancode_to_ascii(scancode);
                        e.Control = kb_state.ctrl_pressed;
                        e.Shift   = kb_state.shift_pressed;
                        e.Alt     = kb_state.alt_pressed;
                        e.NumLck  = kb_state.num_lock;
                        e.ScrLck  = kb_state.scroll_lock;
                        user_callback(e);
                    }
                    break;
            }
            return;
        }
        
        switch (scancode) {
            case LSHIFT_SCANCODE:
            case RSHIFT_SCANCODE:
                kb_state.shift_pressed = pressed;
                break;
            case LCTRL_SCANCODE:
                kb_state.ctrl_pressed = pressed;
                break;
            case LALT_SCANCODE:
                kb_state.alt_pressed = pressed;
                break;
            case CAPS_LOCK_SCANCODE:
                if (pressed) {
                    kb_state.caps_lock = !kb_state.caps_lock;
                    set_leds();
                }
                break;
            case NUM_LOCK_SCANCODE:
                if (pressed) {
                    kb_state.num_lock = !kb_state.num_lock;
                    set_leds();
                }
                break;
            case SCROLL_LOCK_SCANCODE:
                if (pressed) {
                    kb_state.scroll_lock = !kb_state.scroll_lock;
                    set_leds();
                }
                break;
            default:
                if (user_callback != nullptr) {
                    keyboard_event_t e = {0};
                    e.type    = pressed ? KEY_PRESS : KEY_RELEASE;
                    e.KeyCode = scancode;
                    e.KeyChar = scancode_to_ascii(scancode);
                    e.Control = kb_state.ctrl_pressed;
                    e.Shift   = kb_state.shift_pressed;
                    e.Alt     = kb_state.alt_pressed;
                    e.NumLck  = kb_state.num_lock;
                    e.ScrLck  = kb_state.scroll_lock;
                    user_callback(e);
                }
                break;
        }
    }

    UINT8 scancode_to_ascii(UINT8 scancode) {
        if (scancode >= sizeof(scancode_to_ascii_en)) {
            return 0;
        }
        
        char c;
        if (kb_state.shift_pressed) {
            c = scancode_to_ascii_en_shift[scancode];
        } else {
            c = scancode_to_ascii_en[scancode];
        }
        
        if (c >= 'a' && c <= 'z') {
            if (kb_state.caps_lock) {
                c = c - 'a' + 'A';
            }
        } else if (c >= 'A' && c <= 'Z') {
            if (kb_state.caps_lock && !kb_state.shift_pressed) {
                c = c - 'A' + 'a';
            }
        }
        
        return c;
    }

    UINT8 extended_scancode_to_ascii(UINT8 scancode) {
        switch (scancode) {
            case 0x1C: return '\n';    // NumPad Enter
            case 0x35: return '/';     // NumPad /
            case 0x48: return 0;       // Up Arrow
            case 0x50: return 0;       // Down Arrow
            case 0x4B: return 0;       // Left Arrow
            case 0x4D: return 0;       // Right Arrow
            case 0x49: return 0;       // Page Up
            case 0x51: return 0;       // Page Down
            case 0x47: return 0;       // Home
            case 0x4F: return 0;       // End
            case 0x52: return 0;       // Insert
            case 0x53: return 0x7F;    // Delete
            default: return 0;
        }
    }
} // namespace