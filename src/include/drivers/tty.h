#ifndef TTY_H
#define TTY_H

#include "../cpu/types.h"
#include "../../sdk/include/abi/keyboard.h"
#include "../../sdk/include/abi/termios.h"
#include "../../sdk/include/abi/process.h"   // pid_t, signal numbers

// Console terminal: the keyboard event ring, termios, the canonical line
// discipline, the raw-mode key encoder and the foreground process group.
// One tty exists (/dev/tty == /dev/console).
//
// The IRQ-side producer is installed as the keyboard callback while a
// session runs; the consumer side runs in syscall context. read() itself
// never sleeps: it returns -EAGAIN, and the syscall sleeps in wait_readable
// (or wait_key) and reads again. The partially typed line lives here (not
// in the Process), so a read restarted after a signal keeps editing the
// same line.
//
// Events, not bytes, sit in the ring. Canonical mode needs to know which
// key was pressed - an arrow moves the cursor within the line rather than
// inserting anything - so converting to bytes at the IRQ would throw away
// what the line editor needs and force it to re-parse its own escapes.
// Raw mode encodes on the way out instead.

namespace tty
{
    // Reset the ring, the line buffer and termios (session start).
    void reset();

    // Producer side (called from the keyboard IRQ): a key typed on screen
    // `screen`, queued for that screen's reader. The line discipline reads
    // screen 1's (0).
    void on_key(keyboard_event_t e, uint32_t screen = 0);


    // The foreground process group: who ^C goes to, and who is allowed to
    // read the keyboard. 0 means nobody has claimed the terminal, which is
    // how a session starts and what makes the check permissive by default.
    pid_t fg_pgrp();
    void  set_fg_pgrp(pid_t pgid);

    // Is there anything to wake a reader for? Accounts for the mode: a
    // canonical reader waits for a complete line, a raw one for VMIN bytes
    // or the VTIME deadline.
    bool readable();

    // Raw event interface (SYSX_READ_KEY, SfConsole): pop one event queued
    // for `screen`. Returns false when there is none.
    bool pop_key(keyboard_event_t* out, uint32_t screen = 0);

    // Sleep until read() can make progress (readable(), including the
    // VTIME deadline) or until pop_key has an event. False when a signal
    // ended the sleep.
    bool wait_readable();
    // wait_key also ends once stop(screen), when given, is true; whoever
    // changes what it answers calls wake_key_waiters.
    bool wait_key(uint32_t screen = 0, bool (*stop)(uint32_t) = nullptr);
    void wake_key_waiters();

    // Read at most n bytes into dst.
    //   > 0   bytes read
    //   0     EOF (Ctrl+D on an empty canonical line), or a raw read that
    //         timed out with nothing pending
    //   -EAGAIN nothing ready yet (caller sleeps in wait_readable)
    sint64_t read(void* dst, uint64_t n);

    // Write raw bytes to the console (screen + uart). Returns n.
    uint64_t write(const void* src, uint64_t n);

    // termios state. TCSETS validates nothing: the settings are advisory
    // and an application is free to ask for a combination we ignore.
    void get_termios(struct termios* out);
    void set_termios(const struct termios* in);

    // Full-fidelity key reporting (CSI u). Enabled by the application
    // writing ESC [ > 1 u and disabled by ESC [ < u, which the terminal
    // emulator forwards here. Classic xterm sequences cannot express
    // Ctrl+digit, Ctrl+Shift+letter or Alt+symbol; this can.
    void set_csi_u(bool on);
    bool csi_u();

    // Diagnostics: pending canonical line length.
    uint32_t line_len();
}

#endif // TTY_H
