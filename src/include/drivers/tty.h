#ifndef TTY_H
#define TTY_H

#include "../cpu/types.h"
#include "../../sdk/include/abi/keyboard.h"

// The keyboard's side of the screens: every screen has a queue of key
// events, filled from the keyboard IRQ and read by the program that owns
// the screen's input (sfconsole.cpp). Events, not bytes: a line editor needs
// to know which key was pressed - an arrow moves the cursor within the line
// rather than typing anything.

namespace tty
{
    // Producer side (called from the keyboard IRQ): a key typed on screen
    // `screen`, queued for that screen's reader.
    void on_key(keyboard_event_t e, uint32_t screen = 0);

    // Pop one event queued for `screen`. Returns false when there is none.
    bool pop_key(keyboard_event_t* out, uint32_t screen = 0);

    // Sleep until `screen` has an event, or stop(screen), when given, is
    // true; whoever changes what it answers calls wake_key_waiters. False
    // when the process is to be ended.
    bool wait_key(uint32_t screen = 0, bool (*stop)(uint32_t) = nullptr);
    void wake_key_waiters();

    // Write bytes to the selected screen and the serial log. Returns n.
    uint64_t write(const void* src, uint64_t n);
}

#endif // TTY_H
