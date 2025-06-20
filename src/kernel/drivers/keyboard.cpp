#include "../../include/drivers/keyboard.h"

namespace keyboard {
    static keyboard_buffer_t kb_buffer = {0};
    static keyboard_state_t  kb_state  = {0};

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

    static void wait_input(void) {
        while (port::byte_in(KEYBOARD_STATUS_PORT) & 0x02);
    }

    static void wait_output(void) {
        while (!(port::byte_in(KEYBOARD_STATUS_PORT) & 0x01));
    }

    static void send_command(UINT8 command) {
        wait_input();
        port::byte_out(KEYBOARD_COMMAND_PORT, command);
    }

    static void send_data(UINT8 data) {
        wait_input();
        port::byte_out(KEYBOARD_DATA_PORT, data);
    }

    static UINT8 read_data(void) {
        wait_output();
        return port::byte_in(KEYBOARD_DATA_PORT);
    }

    static void set_leds(void) {
        UINT8 led_state = 0;
        
        if (kb_state.caps_lock) led_state |= 0x04;
        if (kb_state.num_lock) led_state |= 0x02;
        if (kb_state.scroll_lock) led_state |= 0x01;
        
        send_data(0xED);
        read_data();
        send_data(led_state);
        read_data();
    }

    static void buffer_put(char c) {
        if (kb_buffer.count < KEYBOARD_BUFFER_SIZE) {
            kb_buffer.buffer[kb_buffer.head] = c;
            kb_buffer.head = (kb_buffer.head + 1) % KEYBOARD_BUFFER_SIZE;
            kb_buffer.count++;
        }
    }

    static char buffer_get(void) {
        if (kb_buffer.count == 0) {
            return 0;
        }
        
        char c = kb_buffer.buffer[kb_buffer.tail];
        kb_buffer.tail = (kb_buffer.tail + 1) % KEYBOARD_BUFFER_SIZE;
        kb_buffer.count--;
        return c;
    }

    void init(void) {
        kb_state.num_lock = 1;
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
                if (pressed) {
                    char c = scancode_to_ascii(scancode);
                    if (c) {
                        buffer_put(c);
                    }
                    // JUST FOR TEST (I HOPE)
                    char t[2];
                    t[0] = c;
                    t[1] = '\0';
                    print(t);
                }
                break;
        }
    }

    char getchar(void) {
        return buffer_get();
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
} // namespace