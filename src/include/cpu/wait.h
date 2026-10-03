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

// A lock whose holder may sleep - wait for a disk - while whoever else
// wants it sleeps until its turn: they get it in the order they asked
// (tickets), so nobody taking it again and again keeps the others out.
// The holder may take it again (depth). A thread holding one, or waiting
// for it, is not ended where it is: Ctrl+Alt+C and the like end it once
// it has let go. Where no thread runs yet (boot) it is taken without
// waiting.
struct sleep_lock
{
    process::Thread* owner = nullptr;
    uint32_t         depth = 0;
    wait_queue       wq;
    uint64_t         next = 0;      // the ticket the next one asking gets
    uint64_t         serving = 0;   // whose turn it is
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

    // wait_event for a condition with no queue of its own - any of several
    // objects (SfSync WaitAny): every wake_up, on whatever queue, wakes it
    // to look again.
    bool wait_any(bool (*cond)(void*), void* arg, uint64_t tick);

    // May what runs now sleep? A thread with interrupts on, outside the
    // scheduler: not an interrupt handler, not the boot or idle task.
    bool can_sleep();

    // A thread long in the kernel (a disk write) gives the CPU to the
    // others for a turn, once its time slice is up. Nothing preempts the
    // kernel: on one CPU, nothing else ran - keys included - until it slept.
    void yield_if_due();

    // Take l, sleeping while another thread has it; false when it could
    // not be taken (held elsewhere, and this cannot sleep): the caller
    // goes on without it and must not unlock.
    bool lock(sleep_lock* l);
    void unlock(sleep_lock* l);

    // In the middle of something long under l (taken once, not nested):
    // when others wait for it, let them have it, then take it back. The
    // caller must be at a point where what l guards is whole. It is not
    // ended meanwhile (as if it still held l).
    void pass(sleep_lock* l);

    // Nothing in between may sleep: what would has to put it off.
    struct no_sleep
    {
        no_sleep();
        ~no_sleep();
    };
}

#endif // WAIT_H
