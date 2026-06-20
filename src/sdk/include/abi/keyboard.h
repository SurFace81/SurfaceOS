#ifndef ABI_KEYBOARD_H
#define ABI_KEYBOARD_H

#include "types.h"

enum key_event_type
{
    KEY_PRESS   = 0,
    KEY_RELEASE = 1,
};

enum key_code
{
    KEY_SPACE       = 57,
    KEY_ESCAPE      = 1,
    KEY_BACKSPACE   = 14,
    KEY_ENTER       = 28,
    KEY_ARROW_LEFT  = 203,
    KEY_ARROW_UP    = 200,
    KEY_ARROW_DOWN  = 208,
    KEY_ARROW_RIGHT = 205,
    KEY_DELETE      = 211,
};

struct keyboard_event_t
{
    uint8_t KeyCode;
    char    KeyChar;
    uint8_t type;
    bool    Control;
    bool    Shift;
    bool    Alt;
    bool    NumLck;
    bool    ScrLck;
};

#endif