#ifndef TTY_H
#define TTY_H

#include "../cpu/types.h"
#include "../../sdk/include/abi/keyboard.h"
#include "../../sdk/include/abi/termios.h"

// Console terminal (stage 4): the keyboard event ring, termios, the
// canonical line discipline and the raw-mode key encoder. One tty exists
// (/dev/tty == /dev/console).
//
// The IRQ-side producer is installed as the keyboard callback while a
// session runs; the consumer side runs in syscall context. Blocking stays
// in process.cpp (Wait::Key + syscall restart): the tty never sleeps, it
// only reports whether input is available and assembles it when read. The
// partially typed line lives here (not in the Process), so a restarted read
// syscall keeps editing the same line.
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

    // Producer side (called from the keyboard IRQ).
    void on_key(keyboard_event_t e);

    // The session's interrupt key (Ctrl+C by default) was pressed: the
    // process layer terminates the session at the next ring-3 boundary.
    bool intr_pressed();

    // Is there anything to wake a reader for? Accounts for the mode: a
    // canonical reader waits for a complete line, a raw one for VMIN bytes
    // or the VTIME deadline.
    bool readable();

    // Raw event interface (SYSX_READ_KEY): pop one queued event. Returns
    // false when the ring is empty.
    bool pop_key(keyboard_event_t* out);

    // Read at most n bytes into dst.
    //   > 0   bytes read
    //   0     EOF (Ctrl+D on an empty canonical line), or a raw read that
    //         timed out with nothing pending
    //   -EAGAIN nothing ready yet (caller blocks and restarts)
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
