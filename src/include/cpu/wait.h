#ifndef WAIT_H
#define WAIT_H

#include "types.h"

// Wait queues: a thread sleeps inside the kernel, on its own kernel stack,
// until something wakes it, and then carries on from the call.
//
//   sleep_on(q)           the current thread sleeps until wake_up(q);
//   wake_up(q)            every sleeper on q becomes runnable (IRQ-safe);
//   sleep_until(t)        the current thread sleeps until pit::ticks() >= t;
//   wait_event(q, c, a, t) sleeps on q until c(a) holds (or tick t passes).
//
// A request to end the process (Ctrl+Alt+C, EndProcess) also ends a sleep:
// the call then returns false and the caller gives up. The queue links its
// threads through the thread table (process.cpp).

namespace process { struct Thread; }

struct wait_queue
{
    process::Thread* head = nullptr;
};

namespace wait
{
    // true: woken (possibly spuriously - re-check the condition); false:
    // the process is to be ended.
    bool sleep_on(wait_queue* q);

    void wake_up(wait_queue* q);

    // true: the tick was reached; false: the process is to be ended.
    bool sleep_until(uint64_t tick);

    // Sleep on q until cond(arg) is true, re-checking it after every wakeup.
    // The check and the enqueue happen with interrupts off, so a wake_up
    // from an IRQ in between is not lost. tick != 0 is a deadline: once it
    // passes the call returns true whatever cond says.
    // true: cond held or the deadline passed; false: the process is to be
    // ended.
    bool wait_event(wait_queue* q, bool (*cond)(void*), void* arg, uint64_t tick);
}

#endif // WAIT_H
