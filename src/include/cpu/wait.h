#ifndef WAIT_H
#define WAIT_H

#include "types.h"

// Wait queues: a process sleeps inside the kernel, on its own kernel stack,
// until something wakes it, and then carries on from the call.
//
//   sleep_on(q)       the current process sleeps until wake_up(q);
//   wake_up(q)        every sleeper on q becomes runnable (IRQ-safe);
//   sleep_until(t)    the current process sleeps until pit::ticks() >= t.
//
// A deliverable signal also ends a sleep: the call then returns false and
// the caller decides between EINTR and restarting. Only processes sleep for
// now; the queue links them through the process table (process.cpp).

namespace process { struct Process; }

struct wait_queue
{
    process::Process* head = nullptr;
};

namespace wait
{
    // true: woken by wake_up; false: a signal is pending.
    bool sleep_on(wait_queue* q);

    void wake_up(wait_queue* q);

    // true: the tick was reached; false: a signal is pending.
    bool sleep_until(uint64_t tick);
}

#endif // WAIT_H
