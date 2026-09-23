// Unit tests for the signal core in src/kernel/cpu/signal.cpp.
// Run with tools/sigtest_host.sh.
//
// signal.cpp has no kernel dependencies at all, so no stub file is needed:
// this is the whole harness.

#include "../../src/include/cpu/signal.h"

extern "C" int printf(const char*, ...);

using namespace sig;

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (ok) passed++; else failed++;
}

static void section(const char* s) { printf("\n%s\n", s); }

static signal_state st;

static void fresh()
{
    init(&st);
}

// --- masks -----------------------------------------------------------------

static void t_pending()
{
    section("pending and blocked");

    fresh();
    check("nothing is deliverable to start with", next_deliverable(&st) == 0);

    post(&st, SIGINT);
    check("a posted signal is deliverable", next_deliverable(&st) == SIGINT);

    post(&st, SIGHUP);
    check("the lowest-numbered signal goes first",
          next_deliverable(&st) == SIGHUP);

    clear(&st, SIGHUP);
    check("clearing it exposes the next", next_deliverable(&st) == SIGINT);

    fresh();
    post(&st, SIGINT);
    set_mask(&st, SIG_BLOCK, SIGMASK(SIGINT), nullptr);
    check("a blocked signal is not deliverable", next_deliverable(&st) == 0);
    check("but stays pending", st.pending & SIGMASK(SIGINT));

    set_mask(&st, SIG_UNBLOCK, SIGMASK(SIGINT), nullptr);
    check("unblocking delivers it", next_deliverable(&st) == SIGINT);

    fresh();
    post(&st, SIGKILL);
    set_mask(&st, SIG_BLOCK, ~(sigset_t)0, nullptr);
    check("SIGKILL ignores the blocked mask",
          next_deliverable(&st) == SIGKILL);

    fresh();
    post(&st, SIGSTOP);
    set_mask(&st, SIG_BLOCK, ~(sigset_t)0, nullptr);
    check("so does SIGSTOP", next_deliverable(&st) == SIGSTOP);

    fresh();
    post(&st, 0);
    post(&st, NSIG);
    post(&st, -1);
    check("out-of-range signals are refused", st.pending == 0);
}

static void t_mask()
{
    section("rt_sigprocmask");

    fresh();
    sigset_t old = ~(sigset_t)0;
    set_mask(&st, SIG_BLOCK, SIGMASK(SIGUSR1), &old);
    check("the old mask comes back", old == 0);

    set_mask(&st, SIG_BLOCK, SIGMASK(SIGUSR2), &old);
    check("BLOCK is additive",
          old == SIGMASK(SIGUSR1) &&
          st.blocked == (SIGMASK(SIGUSR1) | SIGMASK(SIGUSR2)));

    set_mask(&st, SIG_SETMASK, SIGMASK(SIGTERM), nullptr);
    check("SETMASK replaces", st.blocked == SIGMASK(SIGTERM));

    set_mask(&st, SIG_UNBLOCK, SIGMASK(SIGTERM), nullptr);
    check("UNBLOCK subtracts", st.blocked == 0);

    set_mask(&st, SIG_SETMASK, ~(sigset_t)0, nullptr);
    check("SIGKILL and SIGSTOP cannot be blocked",
          (st.blocked & SIG_UNCATCHABLE) == 0);

    sigset_t before = st.blocked;
    set_mask(&st, 99, SIGMASK(SIGINT), nullptr);
    check("an unknown `how` changes nothing", st.blocked == before);
}

// --- dispositions ----------------------------------------------------------

static void t_actions()
{
    section("default actions");

    check("SIGCHLD is ignored", default_action(SIGCHLD) == Action::Ign);
    check("SIGWINCH is ignored", default_action(SIGWINCH) == Action::Ign);
    check("SIGCONT resumes", default_action(SIGCONT) == Action::Cont);
    check("SIGTSTP stops", default_action(SIGTSTP) == Action::Stop);
    check("SIGTTIN stops", default_action(SIGTTIN) == Action::Stop);
    check("SIGSEGV dumps core", default_action(SIGSEGV) == Action::Core);
    check("SIGINT terminates", default_action(SIGINT) == Action::Term);
    check("SIGKILL terminates", default_action(SIGKILL) == Action::Term);

    fresh();
    check("SIGCHLD with no handler is discarded", discarded(&st, SIGCHLD));
    check("SIGINT with no handler is not", !discarded(&st, SIGINT));

    st.act[SIGINT].handler = SIG_IGN;
    check("SIG_IGN discards", discarded(&st, SIGINT));

    st.act[SIGCHLD].handler = 0x400000;
    check("a handler means SIGCHLD is no longer discarded",
          !discarded(&st, SIGCHLD));
    check("and it counts as caught", caught(&st, SIGCHLD));

    st.act[SIGKILL].handler = SIG_IGN;
    check("SIGKILL can never be discarded", !discarded(&st, SIGKILL));
}

static void t_exec_fork()
{
    section("execve and fork");

    fresh();
    st.act[SIGINT].handler  = 0x400000;     // caught
    st.act[SIGINT].flags    = SA_RESTART;
    st.act[SIGQUIT].handler = SIG_IGN;      // ignored
    set_mask(&st, SIG_BLOCK, SIGMASK(SIGUSR1), nullptr);
    post(&st, SIGTERM);

    signal_state child;
    inherit(&child, &st);
    check("fork inherits handlers", child.act[SIGINT].handler == 0x400000);
    check("fork inherits the blocked mask", child.blocked == st.blocked);
    check("fork does not inherit pending signals", child.pending == 0);

    reset_on_exec(&st);
    check("exec resets a caught signal to the default",
          st.act[SIGINT].handler == SIG_DFL);
    check("exec clears its flags too", st.act[SIGINT].flags == 0);
    check("exec keeps an ignored signal ignored",
          st.act[SIGQUIT].handler == SIG_IGN);
    check("exec keeps the blocked mask", st.blocked == SIGMASK(SIGUSR1));
    check("exec drops pending signals", st.pending == 0);
}

// --- the frame -------------------------------------------------------------

static void t_frame()
{
    section("the signal frame");

    // The handler is entered as if by `call`: rsp+8 is what is aligned.
    bool aligned = true;
    bool below   = true;
    bool redzone = true;
    for (uint64_t i = 0; i < 64; i++)
    {
        uint64_t rsp = 0x7FFF0000ULL + i;
        uint64_t f   = frame_addr(rsp);
        if ((f + 8) % 16 != 0) aligned = false;
        if (f >= rsp)          below   = false;
        if (f + sizeof(frame) > rsp - 128) redzone = false;
    }
    check("the handler sees the SysV entry alignment", aligned);
    check("the frame is below the interrupted rsp", below);
    check("the red zone is left untouched", redzone);

    cpu_context ctx = {};
    ctx.regs.rax  = 0x1111;
    ctx.regs.rbx  = 0x2222;
    ctx.iret.rip  = 0x400123;
    ctx.iret.rsp  = 0x7FFF0000;
    ctx.iret.rflags = 0x202;

    frame f;
    build_frame(&f, &ctx, SIGMASK(SIGUSR1), 0x401000);
    check("the frame round-trips the context",
          f.ctx.regs.rax == 0x1111 && f.ctx.regs.rbx == 0x2222 &&
          f.ctx.iret.rip == 0x400123);
    check("the old mask is saved", f.old_mask == SIGMASK(SIGUSR1));
    check("the trampoline is the return address", f.restorer_ret == 0x401000);
    check("a built frame validates", check_frame(&f));

    f.magic = 0;
    check("a forged frame does not", !check_frame(&f));
}

static void t_rflags()
{
    section("sigreturn cannot forge RFLAGS");

    check("IF is always set", sanitize_rflags(0) & 0x200);
    check("the reserved bit 1 is always set", sanitize_rflags(0) & 0x2);
    check("IOPL 3 is stripped", (sanitize_rflags(0x3000) & 0x3000) == 0);
    check("NT is stripped", (sanitize_rflags(0x4000) & 0x4000) == 0);
    check("VM is stripped", (sanitize_rflags(0x20000) & 0x20000) == 0);
    check("AC is stripped", (sanitize_rflags(0x40000) & 0x40000) == 0);
    check("the arithmetic flags survive",
          (sanitize_rflags(0xC5) & 0xC5) == 0xC5);
    check("DF survives", sanitize_rflags(0x400) & 0x400);
    check("TF survives, so a debugger can single-step out of a handler",
          sanitize_rflags(0x100) & 0x100);
}

int main()
{
    printf("signal core unit tests\n");

    t_pending();
    t_mask();
    t_actions();
    t_exec_fork();
    t_frame();
    t_rflags();

    printf("\nsignal: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
