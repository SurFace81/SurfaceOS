#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"
#include "context.h"
#include "../../sdk/include/abi/signal.h"

// ---------------------------------------------------------------------------
// Signal core
// ---------------------------------------------------------------------------
//
// Everything here is pure state arithmetic over a signal_state and a
// cpu_context: no page tables, no process table, no uaccess. Delivery
// itself lives in process.cpp, which is the only place that knows how to
// reach a user stack - but the parts that are easy to get quietly wrong
// (mask rules, the default-action table, the frame layout and its
// alignment) are here, where tools/sigtest_host.sh can exercise them on
// the host.
//
// Why a frame on the user stack at all: the handler is ordinary ring-3
// code, so the interrupted state has to be saved somewhere ring 3 can be
// returned to it from. Linux puts it on the user stack and returns through
// a trampoline that issues rt_sigreturn; doing the same means musl's
// sigreturn path works unchanged in stage 6.

namespace sig
{
    // What happens when a signal has no handler.
    enum class Action : uint8_t
    {
        Term,       // terminate the process
        Ign,        // discard
        Core,       // terminate (no core dumps here)
        Stop,       // stop until SIGCONT
        Cont,       // resume a stopped process
    };

    struct signal_state
    {
        sigset_t     pending;
        sigset_t     blocked;
        k_sigaction  act[NSIG];     // indexed by signal number; [0] unused
    };

    // --- state ------------------------------------------------------------

    void init(signal_state* s);

    // execve(2): a caught signal goes back to the default, an ignored one
    // stays ignored, and the blocked mask survives.
    void reset_on_exec(signal_state* s);

    // fork(2): handlers and mask are inherited, pending signals are not.
    void inherit(signal_state* dst, const signal_state* src);

    inline bool valid(int n) { return n > 0 && n < NSIG; }

    void post(signal_state* s, int n);
    void clear(signal_state* s, int n);

    // Lowest-numbered signal that may be delivered now, or 0. SIGKILL and
    // SIGSTOP ignore the blocked mask.
    int next_deliverable(const signal_state* s);

    // True when the signal would do nothing at all: explicitly ignored, or
    // default-ignored with no handler. Such a signal is discarded at post
    // time rather than left pending forever.
    bool discarded(const signal_state* s, int n);

    bool caught(const signal_state* s, int n);

    Action default_action(int n);

    // rt_sigprocmask. SIGKILL/SIGSTOP can never be blocked.
    void set_mask(signal_state* s, int how, sigset_t set, sigset_t* old);

    // --- the frame --------------------------------------------------------

    // Written to the user stack, read back by rt_sigreturn.
    struct frame
    {
        uint64_t    restorer_ret;   // popped by the handler's `ret`
        cpu_context ctx;            // the interrupted state
        sigset_t    old_mask;
        uint64_t    magic;
    };

    const uint64_t FRAME_MAGIC = 0x5346534E47524554ULL;   // "SFSNGRET"

    // Where the frame goes, given the interrupted rsp. Skips the red zone
    // and lands so that the handler sees the SysV entry alignment
    // (rsp % 16 == 8, i.e. rsp + 8 is what is aligned).
    uint64_t frame_addr(uint64_t rsp);

    void build_frame(frame* f, const cpu_context* ctx, sigset_t old_mask,
                     uint64_t restorer);

    bool check_frame(const frame* f);

    // RFLAGS a sigreturn is allowed to install. Ring 3 must not be able to
    // grant itself IOPL, or set NT/VM/RF by returning from a handler.
    uint64_t sanitize_rflags(uint64_t f);
}

#endif // SIGNAL_H
