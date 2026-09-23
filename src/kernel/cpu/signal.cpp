// The signal core: mask arithmetic, the default-action table and the
// sigframe layout. See cpu/signal.h for why this is kept free of every
// kernel dependency.

#include "../../include/cpu/signal.h"

namespace
{
    // RFLAGS bits ring 3 may set for itself: the arithmetic flags, DF, TF
    // and OF. Everything else - IOPL, IF, NT, RF, VM, AC, the ID bit - is
    // the kernel's, and a forged sigframe must not be able to install it.
    const uint64_t RFLAGS_USER_MASK = 0x0000000000000CD5ULL |  // CF PF AF ZF SF
                                      0x0000000000000100ULL |  // TF
                                      0x0000000000000400ULL |  // DF
                                      0x0000000000000800ULL;   // OF
    const uint64_t RFLAGS_FIXED     = 0x0000000000000202ULL;   // IF + bit 1

    const uint64_t RED_ZONE = 128;      // SysV: leaf functions own it
}

namespace sig
{
    void init(signal_state* s)
    {
        s->pending = 0;
        s->blocked = 0;
        for (uint32_t i = 0; i < NSIG; i++)
        {
            s->act[i].handler  = SIG_DFL;
            s->act[i].flags    = 0;
            s->act[i].restorer = 0;
            s->act[i].mask     = 0;
        }
    }

    void reset_on_exec(signal_state* s)
    {
        s->pending = 0;
        for (uint32_t i = 0; i < NSIG; i++)
        {
            // POSIX: the new program inherits ignored signals (it never
            // asked for them and cannot know they were set), but every
            // handler address belonged to the old image and is now gone.
            if (s->act[i].handler != SIG_IGN)
                s->act[i].handler = SIG_DFL;
            s->act[i].flags    = 0;
            s->act[i].restorer = 0;
            s->act[i].mask     = 0;
        }
    }

    void inherit(signal_state* dst, const signal_state* src)
    {
        dst->pending = 0;               // fork(2): the child starts clean
        dst->blocked = src->blocked;
        for (uint32_t i = 0; i < NSIG; i++)
            dst->act[i] = src->act[i];
    }

    void post(signal_state* s, int n)
    {
        if (!valid(n))
            return;
        s->pending |= SIGMASK(n);
    }

    void clear(signal_state* s, int n)
    {
        if (!valid(n))
            return;
        s->pending &= ~SIGMASK(n);
    }

    int next_deliverable(const signal_state* s)
    {
        // SIGKILL and SIGSTOP are delivered however the mask looks.
        sigset_t ready = s->pending & (~s->blocked | SIG_UNCATCHABLE);
        if (!ready)
            return 0;

        for (int n = 1; n < NSIG; n++)
            if (ready & SIGMASK(n))
                return n;
        return 0;
    }

    Action default_action(int n)
    {
        switch (n)
        {
            case SIGCHLD:
            case SIGURG:
            case SIGWINCH:
                return Action::Ign;

            case SIGCONT:
                return Action::Cont;

            case SIGSTOP:
            case SIGTSTP:
            case SIGTTIN:
            case SIGTTOU:
                return Action::Stop;

            case SIGQUIT:
            case SIGILL:
            case SIGTRAP:
            case SIGABRT:
            case SIGBUS:
            case SIGFPE:
            case SIGSEGV:
            case SIGXCPU:
            case SIGXFSZ:
            case SIGSYS:
                return Action::Core;

            default:
                return Action::Term;
        }
    }

    bool caught(const signal_state* s, int n)
    {
        if (!valid(n))
            return false;
        uint64_t h = s->act[n].handler;
        return h != SIG_DFL && h != SIG_IGN;
    }

    bool discarded(const signal_state* s, int n)
    {
        if (!valid(n))
            return true;
        if (n == SIGKILL || n == SIGSTOP)
            return false;               // never ignorable
        if (s->act[n].handler == SIG_IGN)
            return true;
        return s->act[n].handler == SIG_DFL && default_action(n) == Action::Ign;
    }

    void set_mask(signal_state* s, int how, sigset_t set, sigset_t* old)
    {
        if (old)
            *old = s->blocked;

        set &= ~SIG_UNCATCHABLE;        // SIGKILL/SIGSTOP are never blocked

        switch (how)
        {
            case SIG_BLOCK:   s->blocked |= set; break;
            case SIG_UNBLOCK: s->blocked &= ~set; break;
            case SIG_SETMASK: s->blocked = set; break;
            default: break;
        }
        s->blocked &= ~SIG_UNCATCHABLE;
    }

    // --- the frame --------------------------------------------------------

    uint64_t frame_addr(uint64_t rsp)
    {
        uint64_t a = rsp - RED_ZONE - sizeof(frame);
        a &= ~15ULL;
        // The ABI guarantees rsp+8 is 16-byte aligned at a function's first
        // instruction, because the call pushed a return address onto an
        // aligned stack. The handler is entered the same way, so the fake
        // return address has to leave the same residue.
        return a - 8;
    }

    void build_frame(frame* f, const cpu_context* ctx, sigset_t old_mask,
                     uint64_t restorer)
    {
        f->restorer_ret = restorer;
        f->ctx          = *ctx;
        f->old_mask     = old_mask;
        f->magic        = FRAME_MAGIC;
    }

    bool check_frame(const frame* f)
    {
        return f->magic == FRAME_MAGIC;
    }

    uint64_t sanitize_rflags(uint64_t f)
    {
        return (f & RFLAGS_USER_MASK) | RFLAGS_FIXED;
    }
}
