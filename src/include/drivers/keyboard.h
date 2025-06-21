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

#define MOD_SHIFT               0x01
#define MOD_CTRL                0x02
#define MOD_ALT                 0x04
#define MOD_CAPS                0x08
#define MOD_NUM                 0x10
#define MOD_SCROLL              0x20

typedef struct {
    UINT8 shift_pressed;
    UINT8 ctrl_pressed;
    UINT8 alt_pressed;
    UINT8 caps_lock;
    UINT8 num_lock;
    UINT8 scroll_lock;
    UINT8 extended_code;
} keyboard_state_t;

enum keyboard_event_type {
    KEY_PRESS   = 0,
    KEY_RELEASE = 1,
    KEY_REPEAT  = 2,
};

typedef struct {
    UINT8 keyCode;
    char  key;
    UINT8 type;
    UINT8 modifiers;    // reserved, reserved, scroll, num, caps, alt, ctrl, shift
} keyboard_event_t;

typedef void (*keyboard_callback_t)(keyboard_event_t e);

namespace keyboard {
    void  init(void);
    void  handler(void);
    void set_keyboard_callback(keyboard_callback_t callback);
    void del_keyboard_callback(void);
}

#endif