#include "../../include/cpu/syscall.h"
#include "../../include/drivers/screen.h"

extern "C" void return_to_kernel();

struct syscall_regs
{
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp;
    uint64_t rdi, rsi, rdx, rcx, rbx, rax;
};

extern "C" void syscall_dispatch(syscall_regs* regs)
{
    switch (regs->rax)
    {
        case SYS_EXIT:
            return_to_kernel();
            break;

        case SYS_WRITE:
            screen::printf("%s", (const char*)regs->rdi);
            regs->rax = 0;
            break;

        default:
            regs->rax = (uint64_t)-1;
            break;
    }
}