#include "../../include/drivers/keyboard.h"
#include "../../include/cpu/ports.h"
#include "../../include/drivers/screen.h"
#include "../../include/stdlib/stdio.h"

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

static void keyboard_wait_input(void) {
    while (port_byte_in(KEYBOARD_STATUS_PORT) & 0x02);
}

static void keyboard_wait_output(void) {
    while (!(port_byte_in(KEYBOARD_STATUS_PORT) & 0x01));
}

static void keyboard_send_command(UINT8 command) {
    keyboard_wait_input();
    port_byte_out(KEYBOARD_COMMAND_PORT, command);
}

static void keyboard_send_data(UINT8 data) {
    keyboard_wait_input();
    port_byte_out(KEYBOARD_DATA_PORT, data);
}

static UINT8 keyboard_read_data(void) {
    keyboard_wait_output();
    return port_byte_in(KEYBOARD_DATA_PORT);
}

static void keyboard_set_leds(void) {
    UINT8 led_state = 0;
    
    if (kb_state.caps_lock) led_state |= 0x04;
    if (kb_state.num_lock) led_state |= 0x02;
    if (kb_state.scroll_lock) led_state |= 0x01;
    
    keyboard_send_data(0xED);
    keyboard_read_data();
    keyboard_send_data(led_state);
    keyboard_read_data();
}

static void keyboard_buffer_put(char c) {
    if (kb_buffer.count < KEYBOARD_BUFFER_SIZE) {
        kb_buffer.buffer[kb_buffer.head] = c;
        kb_buffer.head = (kb_buffer.head + 1) % KEYBOARD_BUFFER_SIZE;
        kb_buffer.count++;
    }
}

static char keyboard_buffer_get(void) {
    if (kb_buffer.count == 0) {
        return 0;
    }
    
    char c = kb_buffer.buffer[kb_buffer.tail];
    kb_buffer.tail = (kb_buffer.tail + 1) % KEYBOARD_BUFFER_SIZE;
    kb_buffer.count--;
    return c;
}

void keyboard_init(void) {
    kb_state.num_lock = 1;
    keyboard_set_leds();
}

void keyboard_handler(void) {
    UINT8 scancode = port_byte_in(KEYBOARD_DATA_PORT);
    
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
                keyboard_set_leds();
            }
            break;
            
        case NUM_LOCK_SCANCODE:
            if (pressed) {
                kb_state.num_lock = !kb_state.num_lock;
                keyboard_set_leds();
            }
            break;
            
        case SCROLL_LOCK_SCANCODE:
            if (pressed) {
                kb_state.scroll_lock = !kb_state.scroll_lock;
                keyboard_set_leds();
            }
            break;
            
        default:
            if (pressed) {
                char c = keyboard_scancode_to_ascii(scancode);
                if (c) {
                    keyboard_buffer_put(c);
                }
                // JUST FOR TEST (I HOPE)
                char t[2];
                t[0] = c;
                t[1] = '\0';
                print_str(t);
            }
            break;
    }
}

char keyboard_getchar(void) {
    return keyboard_buffer_get();
}

UINT8 keyboard_scancode_to_ascii(UINT8 scancode) {
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