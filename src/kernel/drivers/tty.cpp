// The keyboard's side of the screens: a queue of key events per screen,
// and the console output. See tty.h.

#include "../../include/drivers/tty.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/uart.h"
#include "../../include/cpu/wait.h"

namespace
{
    const uint32_t RING_SIZE = 128;     // key events (press + release)

    // Key events, one queue per screen: what is typed on a screen waits for
    // that screen's reader (sfconsole.cpp).
    struct KeyRing
    {
        keyboard_event_t ev[RING_SIZE];
        volatile uint32_t head;
        volatile uint32_t tail;
    };
    KeyRing rings[TERM_ALL_SCREENS];

    // Readers sleeping in wait_key, woken by every key and by whoever
    // changes what a reader's stop condition answers.
    wait_queue input_wq;

    inline bool ring_empty(uint32_t screen)
    {
        return rings[screen].head == rings[screen].tail;
    }

    struct KeyWait
    {
        uint32_t screen;
        bool   (*stop)(uint32_t);
    };

    bool key_or_stop(void* arg)
    {
        KeyWait* w = (KeyWait*)arg;
        return !ring_empty(w->screen) || (w->stop && w->stop(w->screen));
    }
}

namespace tty
{
    void on_key(keyboard_event_t e, uint32_t screen)
    {
        // Ctrl+C and the like are keys like any other: what they mean is up
        // to the program (in a ReadLine, Ctrl+C ends the line -
        // sfconsole.cpp). The system's own keys never get here
        // (keyboard.cpp).
        if (screen >= TERM_ALL_SCREENS)
            return;
        KeyRing* r = &rings[screen];
        uint32_t next = (r->head + 1) % RING_SIZE;
        if (next == r->tail)
            return;                 // full: drop

        r->ev[r->head] = e;
        r->head = next;
        wait::wake_up(&input_wq);
    }

    bool pop_key(keyboard_event_t* out, uint32_t screen)
    {
        if (screen >= TERM_ALL_SCREENS)
            return false;
        KeyRing* r = &rings[screen];
        if (r->head == r->tail)
            return false;
        *out = r->ev[r->tail];
        r->tail = (r->tail + 1) % RING_SIZE;
        return true;
    }

    bool wait_key(uint32_t screen, bool (*stop)(uint32_t))
    {
        if (screen >= TERM_ALL_SCREENS)
            return false;
        KeyWait w = { screen, stop };
        return wait::wait_event(&input_wq, key_or_stop, &w, 0);
    }

    void wake_key_waiters()
    {
        wait::wake_up(&input_wq);
    }

    uint64_t write(const void* src, uint64_t n)
    {
        screen::write((const char*)src, n);
        uart::write((const char*)src, n);
        return n;
    }
}
