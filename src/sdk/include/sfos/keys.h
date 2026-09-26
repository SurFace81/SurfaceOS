#ifndef SFOS_KEYS_H
#define SFOS_KEYS_H

// What Console->ReadKey reports: which key (Code), the character it types
// (Char, 0 for none - arrows, F keys, modifiers alone) and the modifiers
// held (Mods). A key that types a character needs no code of its own: look
// at Char. Ctrl with a letter types 1..26 (Ctrl+C is 3).

#define SF_KEY_ESCAPE       1
#define SF_KEY_BACKSPACE    14
#define SF_KEY_TAB          15
#define SF_KEY_ENTER        28
#define SF_KEY_SPACE        57
#define SF_KEY_F1           59
#define SF_KEY_F2           60
#define SF_KEY_F3           61
#define SF_KEY_F4           62
#define SF_KEY_F5           63
#define SF_KEY_F6           64
#define SF_KEY_F7           65
#define SF_KEY_F8           66
#define SF_KEY_F9           67
#define SF_KEY_F10          68
#define SF_KEY_F11          87
#define SF_KEY_F12          88
#define SF_KEY_KP_ENTER     156
#define SF_KEY_HOME         199
#define SF_KEY_UP           200
#define SF_KEY_PAGE_UP      201
#define SF_KEY_LEFT         203
#define SF_KEY_RIGHT        205
#define SF_KEY_END          207
#define SF_KEY_DOWN         208
#define SF_KEY_PAGE_DOWN    209
#define SF_KEY_INSERT       210
#define SF_KEY_DELETE       211

// Mods
#define SF_MOD_SHIFT        0x01
#define SF_MOD_CTRL         0x02
#define SF_MOD_ALT          0x04

#endif // SFOS_KEYS_H
