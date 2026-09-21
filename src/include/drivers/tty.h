#ifndef TTY_H
#define TTY_H

#include "../cpu/types.h"
#include "../../sdk/include/abi/keyboard.h"
#include "../../sdk/include/abi/termios.h"

// Console terminal (stage 3.5): the keyboard event ring, canonical-mode
// line discipline and echo. One tty exists (/dev/tty == /dev/console);
// stage 4 adds raw mode and termios on top of this structure.
//
// The IRQ-side producer is install()-ed as the keyboard callback while a
// session runs; the consumer side runs in syscall context. Blocking stays
// in process.cpp (Wait::Key + syscall restart): the tty never sleeps, it
// only reports whether input is available and assembles lines from the ring
// when read. The partially typed line lives here (not in the Process), so a
// restarted read syscall keeps editing the same line.

namespace tty
{
    // Reset the ring and the line buffer (session start).
    void reset();

    // Producer side (called from the keyboard IRQ).
    void on_key(keyboard_event_t e);

    // Esc was pressed while a session runs: the process layer acts on it.
    bool esc_pressed();

    // Is there anything to wake a reader for (keys queued)?
    bool has_input();

    // Raw event interface (SYSX_READ_KEY until stage 4): pop one queued
    // event. Returns false when the ring is empty.
    bool pop_key(keyboard_event_t* out);

    // Canonical read of at most n bytes into dst.
    //   > 0   bytes of one completed line (including '\n')
    //   0     EOF: Ctrl+D on an empty line
    //   -EAGAIN no completed line yet (caller blocks and restarts)
    // Echoes as it consumes. Short reads are the caller's business.
    sint64_t read(void* dst, uint64_t n);

    // Write raw bytes to the console (screen + uart). Returns n.
    uint64_t write(const void* src, uint64_t n);

    // Fixed termios for TCGETS (stage 3.5: canonical + echo, 8N1-ish).
    void fill_termios(struct termios* out);

    // Diagnostics: pending line length.
    uint32_t line_len();
}

#endif // TTY_H
