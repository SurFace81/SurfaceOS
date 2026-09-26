#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

// Spin locks: a CPU that finds one taken waits on the spot. For short
// stretches only - nothing may sleep while holding one.
//
// The _irq variants also keep this CPU's interrupts off while it is held,
// for data an interrupt handler touches too; they return the flags to
// restore.

struct spinlock
{
    volatile uint32_t locked = 0;
};

namespace spin
{
    void     lock(spinlock* l);
    void     unlock(spinlock* l);
    uint64_t lock_irq(spinlock* l);
    void     unlock_irq(spinlock* l, uint64_t flags);
}

// The big kernel lock: one CPU at a time runs kernel code. It belongs to a
// CPU, not a thread, and nests (an interrupt taken inside the kernel just
// goes one deeper).
//
//   taken    on every entry from ring 3 (interrupt, exception, int 0x80,
//            syscall), at boot, and by the idle loop when it wakes up;
//   dropped  on every return to ring 3 (the entry's own, or a new thread's
//            first one in task_user_start), and by the idle loop before it
//            halts.
//
// A switch between threads happens inside the kernel and leaves the lock
// with the CPU: the thread that resumes goes on holding it.
namespace bkl
{
    void enter();
    void leave();

    // Complain (serial log) unless this CPU holds the lock exactly once -
    // how it must be at a thread switch.
    void check_switch();
}

// task_user_start's way to drop the lock before a new thread's first iret.
extern "C" void bkl_leave_to_user();

#endif // SPINLOCK_H
