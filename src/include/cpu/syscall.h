#ifndef SYSCALL_H
#define SYSCALL_H

#include "../../sdk/include/abi/syscall.h"
#include "../../sdk/include/abi/types.h"

extern "C" void syscall_entry();

// Handler signature: arguments in regs (rdi, rsi, rdx, r10, r8, r9), the
// result (sint64_t, -errno on failure) goes into regs->rax. Blocking
// handlers may rewrite regs/iret via the process layer.
struct user_regs;
struct iret_frame;
typedef void (*syscall_handler_t)(user_regs*, iret_frame*);

namespace syscall
{
    // Build the dispatch tables. Called once from kmain.
    void init();

    // Register one Linux-numbered handler (sys_fs.cpp fills the file group).
    void set_handler(uint32_t nr, syscall_handler_t h);
}

#endif
