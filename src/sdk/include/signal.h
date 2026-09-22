#ifndef SFOS_SIGNAL_H
#define SFOS_SIGNAL_H

#include "abi/types.h"
#include "abi/signal.h"
#include "abi/process.h"    // pid_t

// POSIX signals. The kernel-facing structure is `struct k_sigaction` in
// abi/signal.h; this is the userspace one, in the order POSIX declares it.
// The trampoline in sa_restorer is filled in by sigaction() itself - a
// handler with no way back into the kernel could never return.

typedef void (*sighandler_t)(int);

struct sigaction
{
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    int          sa_flags;
    void       (*sa_restorer)(void);
};

// Set a handler, keeping the old one. SA_RESTART is *not* implied: use
// sigaction() if an interrupted read() should resume rather than fail with
// EINTR. (signal() below does imply it, as every modern libc does.)
int sigaction(int sig, const struct sigaction* act, struct sigaction* old);

// BSD semantics, which is what portable code expects: the handler stays
// installed and interrupted syscalls restart.
sighandler_t signal(int sig, sighandler_t handler);

int sigprocmask(int how, const sigset_t* set, sigset_t* old);
int sigpending(sigset_t* set);
int sigsuspend(const sigset_t* mask);
int pause();
int raise(int sig);

int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int sig);
int sigdelset(sigset_t* set, int sig);
int sigismember(const sigset_t* set, int sig);

#endif
