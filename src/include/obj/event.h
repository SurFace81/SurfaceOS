#ifndef EVENT_H
#define EVENT_H

#include "object.h"

// Events and waiting on kernel objects (roadmap stage 3).
//
// An event is a flag a waiter can sleep on: set() raises it and wakes every
// waiter, reset() lowers it. An auto-reset event lowers itself again when a
// wait on it succeeds, so each set() lets exactly one waiter through.
//
// objects::wait is the one way to wait for any waitable object - an event,
// a process (signaled once it has exited) - optionally with a deadline.

namespace event
{
    // A new event object, not set; the caller holds its reference.
    // nullptr when the pool is exhausted.
    kobject* create(bool auto_reset);

    // Safe from interrupt handlers.
    void set(kobject* o);
    void reset(kobject* o);
}

namespace objects
{
    // Sleep until `o` is signaled. tick != 0: give up once pit::ticks()
    // reaches it. The caller must hold a reference to o.
    //   0           signaled (an auto-reset event has been consumed)
    //   -EINVAL     the object cannot be waited on
    //   -ETIMEDOUT  the deadline passed first
    //   -EINTR      a signal ended the wait
    sint64_t wait(kobject* o, uint64_t tick);

    // The same for the object behind handle h of table t; -EBADF for a free
    // slot.
    sint64_t wait_handle(handle_table* t, sint32_t h, uint64_t tick);
}

#endif // EVENT_H
