#ifndef ABI_SIGNAL_H
#define ABI_SIGNAL_H

#include "types.h"

// Signal ABI, shared between the kernel and the SDK.
//
// The layouts and the numbers are exactly Linux x86_64's, because
// rt_sigaction carries `struct k_sigaction` across the syscall boundary
// verbatim. musl builds its own `struct sigaction` on top of this one in
// stage 6, and it will only work if nothing here is invented.

// --- signal numbers --------------------------------------------------------

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGIOT      6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9       // cannot be caught, blocked or ignored
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19      // cannot be caught, blocked or ignored
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28
#define SIGIO       29
#define SIGPOLL     29
#define SIGPWR      30
#define SIGSYS      31

// Linux's _NSIG: signals are numbered 1..64, real-time ones are not
// implemented but the mask is the full width so musl's sigset_t matches.
#define NSIG        65
#define SIGMASK(n)  (1ULL << ((n) - 1))

typedef uint64_t sigset_t;

// --- handlers --------------------------------------------------------------

#define SIG_DFL     0UL
#define SIG_IGN     1UL
#define SIG_ERR     (~0UL)

// sa_flags
#define SA_NOCLDSTOP    0x00000001  // no SIGCHLD when children stop
#define SA_NOCLDWAIT    0x00000002  // reap children automatically
#define SA_SIGINFO      0x00000004  // three-argument handler (not yet)
#define SA_RESTORER     0x04000000  // sa_restorer holds the trampoline
#define SA_ONSTACK      0x08000000  // alternate stack (not yet)
#define SA_RESTART      0x10000000  // restart syscalls instead of EINTR
#define SA_NODEFER      0x40000000  // do not block the signal in its handler
#define SA_RESETHAND    0x80000000  // reset to SIG_DFL on delivery

// sigprocmask / rt_sigprocmask
#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

// What rt_sigaction passes. Not `struct sigaction`: the userspace type has
// the same fields in a different order, and the SDK converts.
struct k_sigaction
{
    uint64_t handler;       // SIG_DFL, SIG_IGN or a user function
    uint64_t flags;         // SA_*
    uint64_t restorer;      // trampoline that issues rt_sigreturn
    sigset_t mask;          // blocked for the duration of the handler
};

// The kernel only ever accepts the natural sigset_t width.
#define SIGSET_BYTES    8

// Signals that can never be caught, blocked or ignored.
#define SIG_UNCATCHABLE (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP))

#endif // ABI_SIGNAL_H
