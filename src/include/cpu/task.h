#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "context.h"

// Kernel tasks: a kernel stack plus the stack pointer it was left at.
//
// A task runs until it calls task::switch_to, which saves its callee-saved
// registers and RFLAGS on its own stack and resumes another task where that
// one stopped. There is no preemption at this level: user code is preempted
// by the timer, and the process layer turns that into a switch_to on the
// process's own kernel stack, with the trap frame left in place under it.
//
// Three kinds of task exist:
//   * the boot task - kmain's stack, adopted by task::init; it becomes the
//     idle task once the kernel is up;
//   * kernel tasks - an entry function on a fresh stack (the console);
//   * process tasks - one per process, whose first switch_to lands in ring 3
//     through a cpu_context placed at the top of the stack.
//
// Stacks are owned by whoever prepares the task; this layer never allocates.

struct Task
{
    uint64_t    rsp;        // saved stack pointer while the task is not running
    const char* name;       // for diagnostics
};

namespace task
{
    // Adopt the running boot context of this CPU as task `boot`.
    void init(Task* boot, const char* name);

    // The task running right now on this CPU.
    Task* current();

    // Save the running task and resume `next`. Returns when some other task
    // switches back to the caller. Interrupts are off during the switch;
    // each task gets its own RFLAGS back.
    void switch_to(Task* next);

    // Make `t` start at entry(arg), interrupts on, on the stack that ends at
    // stack_top (16-byte aligned). entry must never return.
    void prepare_kernel(Task* t, const char* name, uint64_t stack_top,
                        void (*entry)(void*), void* arg);

    // Make `t` enter ring 3 with `ctx` on its first run. The context is
    // copied to the top of the stack, which becomes the trap frame the
    // process's later kernel entries return through.
    void prepare_user(Task* t, const char* name, uint64_t stack_top,
                      const cpu_context* ctx);
}

#endif // TASK_H
