#ifndef DRIVERS_KBD_H
#define DRIVERS_KBD_H

#include "../cpu/types.h"
#include "keyevent.h"

// What every keyboard shares, whichever wire it is on: the modifiers and
// locks, the kernel's own keys, the character a key types, and delivery to
// the shown screen. A keyboard driver only turns its wire format into
// physical keys and passes them here.
namespace kbd {
    // A key went down or up. `code` is the physical key as keyevent.h names
    // it: the set-1 scancode, or `scancode | 0x80` for 0xE0-prefixed keys.
    // The plain keypad codes 0x47..0x53 are the keypad itself; whether they
    // type or navigate is decided here, by Num Lock.
    void key(uint8_t code, bool pressed);

    // The lock states as KMOD_CAPS | KMOD_NUM | KMOD_SCROLL, for LEDs.
    uint8_t locks();
}

#endif
