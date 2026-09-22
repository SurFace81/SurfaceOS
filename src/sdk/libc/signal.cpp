#include "../include/signal.h"
#include "../include/syscall.h"
#include "../include/errno.h"
#include "../include/abi/syscall.h"
#include "../include/unistd.h"     // kill/getpid, for raise()

// The trampoline. A signal handler is entered as if by `call`, with the
// kernel having pushed this address as the return address, so the handler's
// own `ret` lands here and the frame the kernel built is still on the
// stack. rt_sigreturn then restores the interrupted context wholesale -
// which is why returning from a handler needs no cooperation from the
// compiler, and why a handler that falls off the end still works.
//
// It has to be written as top-level assembly rather than a C function:
// the kernel finds the frame at a fixed offset from rsp, so a compiler
// prologue moving rsp would put it out of reach. __attribute__((naked))
// is not enough - x86 GCC emits `push %rbp` anyway once the body uses
// extended asm, which is exactly the eight-byte shift that makes the frame
// fail its magic check.
#define SIG_STR_(x) #x
#define SIG_STR(x)  SIG_STR_(x)

asm(".text\n"
    ".globl __sigreturn_trampoline\n"
    ".type __sigreturn_trampoline, @function\n"
    "__sigreturn_trampoline:\n"
    "    mov $" SIG_STR(SYS_RT_SIGRETURN) ", %eax\n"
    "    int $0x80\n"
    "    ud2\n"                 // rt_sigreturn never comes back
    ".size __sigreturn_trampoline, . - __sigreturn_trampoline\n");

extern "C" void __sigreturn_trampoline();

static inline bool bad_signal(int sig)
{
    return sig <= 0 || sig >= NSIG;
}

int sigemptyset(sigset_t* set)  { *set = 0; return 0; }
int sigfillset(sigset_t* set)   { *set = ~(sigset_t)0; return 0; }

int sigaddset(sigset_t* set, int sig)
{
    if (bad_signal(sig)) { errno = EINVAL; return -1; }
    *set |= SIGMASK(sig);
    return 0;
}

int sigdelset(sigset_t* set, int sig)
{
    if (bad_signal(sig)) { errno = EINVAL; return -1; }
    *set &= ~SIGMASK(sig);
    return 0;
}

int sigismember(const sigset_t* set, int sig)
{
    if (bad_signal(sig)) { errno = EINVAL; return -1; }
    return (*set & SIGMASK(sig)) ? 1 : 0;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* old)
{
    k_sigaction ka, ko;
    k_sigaction* kap = nullptr;

    if (act)
    {
        ka.handler  = (uint64_t)act->sa_handler;
        ka.flags    = (uint64_t)(uint32_t)act->sa_flags | SA_RESTORER;
        ka.restorer = (uint64_t)&__sigreturn_trampoline;
        ka.mask     = act->sa_mask;
        kap = &ka;
    }

    sint64_t r = syscall(SYS_RT_SIGACTION, (uint64_t)sig, (uint64_t)kap,
                         (uint64_t)(old ? &ko : nullptr), SIGSET_BYTES);
    if (r < 0)
    {
        errno = (int)-r;
        return -1;
    }

    if (old)
    {
        old->sa_handler  = (sighandler_t)ko.handler;
        old->sa_mask     = ko.mask;
        old->sa_flags    = (int)(uint32_t)ko.flags;
        old->sa_restorer = (void (*)(void))ko.restorer;
    }
    return 0;
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, old;
    act.sa_handler  = handler;
    act.sa_mask     = 0;
    act.sa_flags    = SA_RESTART;
    act.sa_restorer = nullptr;

    if (sigaction(sig, &act, &old) < 0)
        return (sighandler_t)SIG_ERR;
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    return (int)__syscall_ret(syscall(SYS_RT_SIGPROCMASK, (uint64_t)how,
                                      (uint64_t)set, (uint64_t)old,
                                      SIGSET_BYTES));
}

int sigpending(sigset_t* set)
{
    return (int)__syscall_ret(syscall(SYS_RT_SIGPENDING, (uint64_t)set,
                                      SIGSET_BYTES));
}

int sigsuspend(const sigset_t* mask)
{
    return (int)__syscall_ret(syscall(SYS_RT_SIGSUSPEND, (uint64_t)mask,
                                      SIGSET_BYTES));
}

int pause()
{
    return (int)__syscall_ret(syscall(SYS_PAUSE));
}

int raise(int sig)
{
    return kill(getpid(), sig);
}
