#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../cpu/types.h"
#include "../cpu/ports.h"
#include "screen.h"
#include "../stdlib/stdio.h"

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

typedef struct {
    uint8_t shift_pressed;
    uint8_t ctrl_pressed;
    uint8_t alt_pressed;
    uint8_t caps_lock;
    uint8_t num_lock;
    uint8_t scroll_lock;
    uint8_t extended_code;
} keyboard_state_t;

enum keyboard_event_type {
    KEY_PRESS   = 0,
    KEY_RELEASE = 1,
    KEY_REPEAT  = 2,
};

enum Keys {
    /// Virtual key codes enum
    SPACE           = 57,
    ESCAPE          = 1,
    BACKSPACE       = 14,
    ENTER           = 28,
    ARROW_LEFT      = 203,
    ARROW_UP        = 200,
    ARROW_DOWN      = 208,
    ARROW_RIGHT     = 205,
    DELETE          = 211,
};

typedef struct {
    uint8_t KeyCode;
    char  KeyChar;
    uint8_t type;
    bool  Control;
    bool  Shift;
    bool  Alt;
    bool  NumLck;
    bool  ScrLck;
} keyboard_event_t;

typedef void (*keyboard_callback_t)(keyboard_event_t e);

namespace keyboard {
    void  init(void);
    void  handler(void);
    void set_keyboard_callback(keyboard_callback_t callback);
    void del_keyboard_callback(void);
    keyboard_callback_t get_callback();
}

#endif