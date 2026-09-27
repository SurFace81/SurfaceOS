#ifndef ABI_KEYBOARD_H
#define ABI_KEYBOARD_H

#include "types.h"

enum key_event_type
{
    KEY_PRESS   = 0,
    KEY_RELEASE = 1,
};

// Key codes are PS/2 set-1 scancodes. Keys behind the 0xE0 prefix are
// reported as `scancode | 0x80`; plain scancodes never exceed 0x58, so the
// two ranges cannot collide. Pause has no single scancode and gets 255.
enum key_code
{
    KEY_ESCAPE      = 1,
    KEY_1           = 2,
    KEY_MINUS       = 12,
    KEY_EQUALS      = 13,
    KEY_BACKSPACE   = 14,
    KEY_TAB         = 15,
    KEY_Q           = 16,
    KEY_LBRACKET    = 26,
    KEY_RBRACKET    = 27,
    KEY_ENTER       = 28,
    KEY_LEFT_CTRL   = 29,
    KEY_A           = 30,
    KEY_SEMICOLON   = 39,
    KEY_APOSTROPHE  = 40,
    KEY_BACKTICK    = 41,
    KEY_LEFT_SHIFT  = 42,
    KEY_BACKSLASH   = 43,
    KEY_Z           = 44,
    KEY_COMMA       = 51,
    KEY_PERIOD      = 52,
    KEY_SLASH       = 53,
    KEY_RIGHT_SHIFT = 54,
    KEY_KP_STAR     = 55,
    KEY_LEFT_ALT    = 56,
    KEY_SPACE       = 57,
    KEY_CAPS_LOCK   = 58,

    KEY_F1          = 59,
    KEY_F2          = 60,
    KEY_F3          = 61,
    KEY_F4          = 62,
    KEY_F5          = 63,
    KEY_F6          = 64,
    KEY_F7          = 65,
    KEY_F8          = 66,
    KEY_F9          = 67,
    KEY_F10         = 68,
    KEY_NUM_LOCK    = 69,
    KEY_SCROLL_LOCK = 70,

    // Keypad, reported when Num Lock is in effect.
    KEY_KP_7        = 71,
    KEY_KP_8        = 72,
    KEY_KP_9        = 73,
    KEY_KP_MINUS    = 74,
    KEY_KP_4        = 75,
    KEY_KP_5        = 76,
    KEY_KP_6        = 77,
    KEY_KP_PLUS     = 78,
    KEY_KP_1        = 79,
    KEY_KP_2        = 80,
    KEY_KP_3        = 81,
    KEY_KP_0        = 82,
    KEY_KP_PERIOD   = 83,

    KEY_F11         = 87,
    KEY_F12         = 88,

    // 0xE0-prefixed keys, and the navigation meaning of the keypad when
    // Num Lock is off.
    KEY_KP_ENTER    = 156,      // 0x1C | 0x80
    KEY_RIGHT_CTRL  = 157,      // 0x1D | 0x80
    KEY_KP_SLASH    = 181,      // 0x35 | 0x80
    KEY_PRINT_SCREEN= 183,      // 0x37 | 0x80
    KEY_RIGHT_ALT   = 184,      // 0x38 | 0x80
    KEY_HOME        = 199,      // 0x47 | 0x80
    KEY_ARROW_UP    = 200,      // 0x48 | 0x80
    KEY_PAGE_UP     = 201,      // 0x49 | 0x80
    KEY_ARROW_LEFT  = 203,      // 0x4B | 0x80
    KEY_ARROW_RIGHT = 205,      // 0x4D | 0x80
    KEY_END         = 207,      // 0x4F | 0x80
    KEY_ARROW_DOWN  = 208,      // 0x50 | 0x80
    KEY_PAGE_DOWN   = 209,      // 0x51 | 0x80
    KEY_INSERT      = 210,      // 0x52 | 0x80
    KEY_DELETE      = 211,      // 0x53 | 0x80
    KEY_LEFT_WIN    = 219,      // 0x5B | 0x80
    KEY_RIGHT_WIN   = 220,      // 0x5C | 0x80
    KEY_MENU        = 221,      // 0x5D | 0x80

    KEY_PAUSE       = 255,      // E1 1D 45 E1 9D C5, no release
};

// Modifier bits in keyboard_event_t::Mods.
#define KMOD_SHIFT      0x01
#define KMOD_CTRL       0x02
#define KMOD_ALT        0x04
#define KMOD_ALTGR      0x08    // right Alt, reported alongside KMOD_ALT
#define KMOD_CAPS       0x10    // lock state, not a held key
#define KMOD_NUM        0x20
#define KMOD_SCROLL     0x40

struct keyboard_event_t
{
    uint8_t KeyCode;
    char    KeyChar;        // 0 when the key produces no character
    uint8_t type;
    bool    Control;
    bool    Shift;
    bool    Alt;
    bool    NumLck;
    bool    ScrLck;
    bool    CapsLck;
    uint8_t Mods;           // KMOD_*, the same information in one byte
};

#endif
