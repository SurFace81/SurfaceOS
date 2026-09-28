#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../cpu/ports.h"
#include "keyevent.h"

#define KEYBOARD_DATA_PORT      0x60
#define KEYBOARD_STATUS_PORT    0x64
#define KEYBOARD_COMMAND_PORT   0x64

#define KEYBOARD_BUFFER_SIZE    256

#define KEY_RELEASED_MASK       0x80

// The scancodes this driver itself has to recognise; the rest are keys.
#define LSHIFT_SCANCODE         0x2A
#define RSHIFT_SCANCODE         0x36
#define EXTENDED_SCANCODE       0xE0
#define PAUSE_PREFIX            0xE1

// PS/2 keyboard: bytes in, physical keys out to kbd (kbd.h).
namespace keyboard {
    void  init(void);
    void  handler(void);
    // Light the LEDs for KMOD_CAPS / KMOD_NUM / KMOD_SCROLL in `locks`.
    void  set_leds(uint8_t locks);
}

#endif