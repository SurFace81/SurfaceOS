#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../cpu/ports.h"
#include "keyevent.h"

#define KEYBOARD_DATA_PORT      0x60
#define KEYBOARD_STATUS_PORT    0x64
#define KEYBOARD_COMMAND_PORT   0x64

#define KEYBOARD_BUFFER_SIZE    256

#define KEY_RELEASED_MASK       0x80

#define LSHIFT_SCANCODE         0x2A
#define RSHIFT_SCANCODE         0x36
#define LCTRL_SCANCODE          0x1D
#define LALT_SCANCODE           0x38
#define CAPS_LOCK_SCANCODE      0x3A
#define NUM_LOCK_SCANCODE       0x45
#define SCROLL_LOCK_SCANCODE    0x46

#define EXTENDED_SCANCODE       0xE0

#define RCTRL_SCANCODE          0x1D    // behind 0xE0
#define RALT_SCANCODE           0x38    // behind 0xE0 (AltGr)
#define PAUSE_PREFIX            0xE1

typedef struct {
    uint8_t lshift_pressed;
    uint8_t rshift_pressed;
    uint8_t lctrl_pressed;
    uint8_t rctrl_pressed;
    uint8_t lalt_pressed;
    uint8_t ralt_pressed;       // AltGr
    uint8_t caps_lock;
    uint8_t num_lock;
    uint8_t scroll_lock;
    uint8_t extended_code;      // 0xE0 seen, next byte completes the key
    uint8_t skip_bytes;         // tail of a multi-byte sequence to swallow
} keyboard_state_t;

namespace keyboard {
    void  init(void);
    void  handler(void);
}

#endif