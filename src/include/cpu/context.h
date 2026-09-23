#ifndef CONTEXT_H
#define CONTEXT_H

#include "types.h"

// The shape of a saved ring-3 CPU state. Split out of process.h so that
// code which only manipulates a context - the signal core, and its host
// unit tests - does not have to drag in paging, the keyboard and the
// process table with it.

// General purpose registers in the order SAVE_REGS pushes them (interrupts.asm):
// rax is pushed first, so it sits at the highest address.
struct user_regs
{
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp;
    uint64_t rdi, rsi, rdx, rcx, rbx, rax;
};

// What the CPU pushes when it enters the kernel from ring 3.
struct iret_frame
{
    uint64_t rip, cs, rflags, rsp, ss;
};

// A complete user-mode CPU state. Laid out exactly like the stack after
// SAVE_REGS in syscall_entry, so process_enter_user can pop it directly.
struct cpu_context
{
    user_regs  regs;
    iret_frame iret;
};

#endif // CONTEXT_H
