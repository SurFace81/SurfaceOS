// PS/2 keyboard, scancode set 1.
//
// This driver only turns the bytes from port 0x60 into physical keys: the
// scancode itself, or `scancode | 0x80` for the 0xE0-prefixed keys, which
// keyevent.h names. What a key means - modifiers, locks, the character it
// types - is kbd.cpp's, shared with every other keyboard.
//
// Two keys are not a single scancode and are handled explicitly:
//   Print Screen  E0 2A E0 37  (press)   E0 B7 E0 AA (release)
//   Pause         E1 1D 45 E1 9D C5      (press only, no release)
// The 0x2A/0xAA halves are a "fake shift" the controller inserts so that
// DOS-era software saw a consistent shift state; they are dropped here.

#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/kbd.h"

namespace keyboard {
    static bool    extended_code = false;   // 0xE0 seen, next byte completes the key
    static uint8_t skip_bytes    = 0;       // tail of a multi-byte sequence to swallow

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

    void set_leds(uint8_t locks) {
        uint8_t led_state = 0;

        if (locks & KMOD_CAPS)      led_state |= 0x04;
        if (locks & KMOD_NUM)       led_state |= 0x02;
        if (locks & KMOD_SCROLL)    led_state |= 0x01;

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

        extended_code = false;
        skip_bytes    = 0;
        set_leds(kbd::locks());
    }

    void handler(void) {
        uint8_t scancode = port::byte_in(KEYBOARD_DATA_PORT);

        // Tail of a multi-byte sequence whose meaning was already reported.
        if (skip_bytes) {
            skip_bytes--;
            return;
        }

        if (scancode == PAUSE_PREFIX) {
            // E1 1D 45 E1 9D C5 arrives in one go and no release ever
            // follows. Report the press now and swallow the remaining five
            // bytes, which would otherwise decode as Ctrl and Num Lock.
            skip_bytes = 5;
            kbd::key(KEY_PAUSE, true);
            return;
        }

        if (scancode == EXTENDED_SCANCODE) {
            extended_code = true;
            return;
        }

        bool pressed = !(scancode & KEY_RELEASED_MASK);
        scancode &= ~KEY_RELEASED_MASK;

        if (extended_code) {
            extended_code = false;
            // The controller brackets Print Screen with a fake shift so that
            // software watching the shift state saw it unchanged. It is not
            // a key, and reporting it would look like a real shift press.
            if (scancode == LSHIFT_SCANCODE || scancode == RSHIFT_SCANCODE)
                return;
            kbd::key((uint8_t)(scancode | 0x80), pressed);
            return;
        }

        kbd::key(scancode, pressed);
    }
} // namespace
