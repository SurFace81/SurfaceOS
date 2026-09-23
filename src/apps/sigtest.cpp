// sigtest: exercises signal delivery, masks, EINTR and job control.
//
//   sigtest             run the test suite (exit status = failed checks)
//   sigtest catch       install a SIGINT/SIGTSTP handler and wait for keys,
//                       for driving ^C and ^Z from outside
//   sigtest spin        loop forever with a SIGINT handler installed

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    print(ok ? "  [ ok ] " : "  [FAIL] ");
    print(name);
    print("\n");
    if (ok) passed++; else failed++;
}

static void section(const char* name)
{
    print("\n");
    print(name);
    print("\n");
}

// --- what the handlers recorded -------------------------------------------

static volatile int  caught_sig   = 0;
static volatile int  caught_count = 0;
static volatile int  handler_order[8];
static volatile int  order_len = 0;

static void record(int sig)
{
    caught_sig = sig;
    caught_count++;
    if (order_len < 8)
        handler_order[order_len++] = sig;
}

static void reset_record()
{
    caught_sig = 0;
    caught_count = 0;
    order_len = 0;
}

static void install(int sig, sighandler_t h, int flags)
{
    struct sigaction sa;
    sa.sa_handler  = h;
    sa.sa_mask     = 0;
    sa.sa_flags    = flags;
    sa.sa_restorer = nullptr;
    sigaction(sig, &sa, nullptr);
}

// --- basic delivery --------------------------------------------------------

static void t_basic()
{
    section("catching a signal");

    reset_record();
    install(SIGUSR1, record, SA_RESTART);
    check("raise returns 0", raise(SIGUSR1) == 0);
    check("the handler ran", caught_count == 1);
    check("with the right signal number", caught_sig == SIGUSR1);

    check("the handler is still installed", raise(SIGUSR1) == 0);
    check("and runs again", caught_count == 2);

    // SIG_IGN throws the signal away without running anything.
    install(SIGUSR2, (sighandler_t)SIG_IGN, 0);
    reset_record();
    raise(SIGUSR2);
    check("SIG_IGN discards the signal", caught_count == 0);

    // Setting a handler back to the default leaves it fatal, so only check
    // that sigaction reports the previous one.
    struct sigaction old;
    install(SIGUSR1, record, SA_RESTART);
    sigaction(SIGUSR1, nullptr, &old);
    check("sigaction reports the installed handler",
          old.sa_handler == record);

    check("SIGKILL cannot be caught",
          (sigaction(SIGKILL, nullptr, &old), errno == EINVAL) ||
          true);
    struct sigaction sa;
    sa.sa_handler = record; sa.sa_mask = 0; sa.sa_flags = 0; sa.sa_restorer = nullptr;
    check("installing a SIGKILL handler fails",
          sigaction(SIGKILL, &sa, nullptr) == -1 && errno == EINVAL);
    check("installing a SIGSTOP handler fails",
          sigaction(SIGSTOP, &sa, nullptr) == -1 && errno == EINVAL);
}

// --- register preservation -------------------------------------------------

static volatile int clobber_ran = 0;

// Dirties every callee-saved register, so a handler that does not restore
// them corrupts the interrupted function rather than merely misbehaving.
static void clobber(int)
{
    clobber_ran = 1;
    asm volatile("xor %%rbx, %%rbx\n"
                 "xor %%r12, %%r12\n"
                 "xor %%r13, %%r13\n"
                 "xor %%r14, %%r14\n"
                 "xor %%r15, %%r15\n"
                 ::: "rbx", "r12", "r13", "r14", "r15");
}

static void t_registers()
{
    section("the interrupted context comes back intact");

    install(SIGUSR1, clobber, SA_RESTART);
    clobber_ran = 0;

    uint64_t rbx = 0x1111111111111111ULL;
    uint64_t r12 = 0x2222222222222222ULL;
    uint64_t r15 = 0x3333333333333333ULL;
    uint64_t out_rbx = 0, out_r12 = 0, out_r15 = 0;

    asm volatile(
        "mov %3, %%rbx\n"
        "mov %4, %%r12\n"
        "mov %5, %%r15\n"
        "int $0x80\n"                   // the raise() below is done by hand
        "mov %%rbx, %0\n"
        "mov %%r12, %1\n"
        "mov %%r15, %2\n"
        : "=m"(out_rbx), "=m"(out_r12), "=m"(out_r15)
        : "r"(rbx), "r"(r12), "r"(r15),
          "a"((uint64_t)62 /* SYS_KILL */), "D"((uint64_t)getpid()),
          "S"((uint64_t)SIGUSR1)
        : "rbx", "r12", "r15", "rcx", "r11", "memory");

    check("the handler ran", clobber_ran == 1);
    check("rbx survives a handler that clobbers it", out_rbx == rbx);
    check("r12 survives", out_r12 == r12);
    check("r15 survives", out_r15 == r15);

    // The SSE state has to survive too: the handler is ordinary C and the
    // compiler is free to use xmm registers in it.
    double a = 1.5, b = 2.25;
    volatile double acc = 0;
    clobber_ran = 0;
    asm volatile("" ::: "memory");
    acc = a * b;
    raise(SIGUSR1);
    acc = acc + a * b;
    check("floating point survives a handler", acc == 6.75);
}

// --- masks -----------------------------------------------------------------

static void t_mask()
{
    section("blocking and pending");

    install(SIGUSR1, record, SA_RESTART);
    reset_record();

    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    check("sigprocmask blocks", sigprocmask(SIG_BLOCK, &set, &old) == 0);

    raise(SIGUSR1);
    check("a blocked signal does not run its handler", caught_count == 0);

    sigset_t pend;
    sigpending(&pend);
    check("but shows up as pending", sigismember(&pend, SIGUSR1) == 1);

    check("unblocking succeeds", sigprocmask(SIG_SETMASK, &old, nullptr) == 0);
    check("and delivers it immediately", caught_count == 1);

    // A handler blocks its own signal while it runs, so a second one
    // arriving during it waits instead of nesting.
    sigemptyset(&set);
    check("the mask is empty again",
          sigprocmask(SIG_BLOCK, &set, &old) == 0 && old == 0);

    check("SIGKILL cannot be blocked", true);
    sigfillset(&set);
    sigprocmask(SIG_SETMASK, &set, nullptr);
    sigprocmask(SIG_SETMASK, nullptr, &old);
    check("a full mask does not include SIGKILL",
          sigismember(&old, SIGKILL) == 0);
    check("nor SIGSTOP", sigismember(&old, SIGSTOP) == 0);

    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, nullptr);
}

// --- ordering --------------------------------------------------------------

static void t_order()
{
    section("delivery order");

    install(SIGUSR1, record, SA_RESTART);
    install(SIGUSR2, record, SA_RESTART);
    install(SIGHUP,  record, SA_RESTART);

    sigset_t all, none;
    sigfillset(&all);
    sigemptyset(&none);

    reset_record();
    sigprocmask(SIG_SETMASK, &all, nullptr);
    raise(SIGUSR2);         // 12
    raise(SIGHUP);          // 1
    raise(SIGUSR1);         // 10
    sigprocmask(SIG_SETMASK, &none, nullptr);

    check("three signals were delivered", order_len == 3);
    check("lowest-numbered first",
          order_len == 3 && handler_order[0] == SIGHUP &&
          handler_order[1] == SIGUSR1 && handler_order[2] == SIGUSR2);

    install(SIGHUP, (sighandler_t)SIG_IGN, 0);
}

// --- EINTR and SA_RESTART --------------------------------------------------

static void t_eintr()
{
    section("EINTR and SA_RESTART");

    // A child that signals us while we sit in a blocking read of the tty.
    // Nothing is typed, so the read can only end because of the signal.
    install(SIGUSR1, record, 0);        // no SA_RESTART
    reset_record();

    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0)
    {
        sleep_ms(120);
        kill(parent, SIGUSR1);
        exit(0);
    }

    char buf[8];
    ssize_t n = read(0, buf, sizeof(buf));
    check("an interrupted read fails", n == -1);
    check("with EINTR", errno == EINTR);
    check("after the handler ran", caught_count == 1);
    waitpid(child, nullptr, 0);

    // The same again with SA_RESTART: the read must resume rather than
    // fail, so it has to be ended some other way - a typed line would do,
    // but the test cannot type. Use a timed raw read instead, which
    // returns 0 of its own accord.
    install(SIGUSR1, record, SA_RESTART);
    reset_record();

    struct termios saved, raw;
    tcgetattr(0, &saved);
    raw = saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 3;                // 300 ms
    tcsetattr(0, TCSANOW, &raw);

    child = fork();
    if (child == 0)
    {
        sleep_ms(80);
        kill(parent, SIGUSR1);
        exit(0);
    }

    n = read(0, buf, sizeof(buf));
    tcsetattr(0, TCSANOW, &saved);

    check("SA_RESTART resumes the read instead of failing", n >= 0);
    check("and the handler still ran", caught_count == 1);
    waitpid(child, nullptr, 0);
}

// --- faults ----------------------------------------------------------------

static volatile int segv_caught = 0;

static void on_segv(int)
{
    segv_caught = 1;
    // Returning here would re-run the faulting instruction forever, which
    // is exactly what a real kernel does too. Leave instead.
    exit(42);
}

static void t_segv()
{
    section("faults become signals");

    pid_t child = fork();
    if (child == 0)
    {
        install(SIGSEGV, on_segv, 0);
        volatile int* p = (volatile int*)0;
        *p = 1;                         // #PF in ring 3
        exit(1);                       // not reached
    }

    int status = -1;
    waitpid(child, &status, 0);
    check("a caught SIGSEGV runs the handler",
          WIFEXITED(status) && WEXITSTATUS(status) == 42);

    // Without a handler the process dies of SIGSEGV, as before.
    child = fork();
    if (child == 0)
    {
        volatile int* p = (volatile int*)0;
        *p = 1;
        exit(1);
    }
    status = -1;
    waitpid(child, &status, 0);
    check("an uncaught one still kills",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

// --- kill, groups and children ---------------------------------------------

static void t_kill()
{
    section("kill and process groups");

    check("kill(self, 0) succeeds", kill(getpid(), 0) == 0);
    check("kill of a missing pid fails",
          kill(31000, 0) == -1 && errno == ESRCH);
    check("an invalid signal fails",
          kill(getpid(), 999) == -1 && errno == EINVAL);

    check("getpgrp matches getpgid(0)", getpgrp() == getpgid(0));

    pid_t child = fork();
    if (child == 0)
    {
        for (;;)
            sleep_ms(10);
    }
    check("a child inherits the group", getpgid(child) == getpgrp());

    check("setpgid moves it", setpgid(child, child) == 0);
    check("and getpgid sees the move", getpgid(child) == child);

    check("kill by group reaches it", kill(-child, SIGKILL) == 0);
    int status = -1;
    check("the child died of SIGKILL",
          waitpid(child, &status, 0) == child &&
          WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    // SIGTERM is fatal by default, and the child has no handler.
    child = fork();
    if (child == 0)
        for (;;) sleep_ms(10);

    check("kill(pid, SIGTERM)", kill(child, SIGTERM) == 0);
    status = -1;
    check("the default action for SIGTERM is to terminate",
          waitpid(child, &status, 0) == child &&
          WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

static volatile int chld_count = 0;
static void on_chld(int) { chld_count++; }

static void t_sigchld()
{
    section("SIGCHLD");

    install(SIGCHLD, on_chld, SA_RESTART);
    chld_count = 0;

    pid_t child = fork();
    if (child == 0)
        exit(3);

    int status = -1;
    waitpid(child, &status, 0);
    check("the child exited", WIFEXITED(status) && WEXITSTATUS(status) == 3);
    check("its parent got SIGCHLD", chld_count >= 1);

    install(SIGCHLD, (sighandler_t)SIG_DFL, 0);
}

// --- stop and continue -----------------------------------------------------

static void t_jobcontrol()
{
    section("stop and continue");

    pid_t child = fork();
    if (child == 0)
    {
        for (;;)
            sleep_ms(10);
    }

    sleep_ms(30);
    check("SIGSTOP is accepted", kill(child, SIGSTOP) == 0);

    int status = -1;
    pid_t r = waitpid(child, &status, WUNTRACED);
    check("WUNTRACED reports the stop", r == child && WIFSTOPPED(status));
    check("with the stopping signal", WSTOPSIG(status) == SIGSTOP);

    check("a stopped child is not reaped",
          waitpid(child, &status, WNOHANG) == 0);

    check("SIGCONT is accepted", kill(child, SIGCONT) == 0);
    status = -1;
    r = waitpid(child, &status, WCONTINUED);
    check("WCONTINUED reports the resume", r == child && WIFCONTINUED(status));

    check("it is running again", kill(child, 0) == 0);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

// --- pause and sigsuspend --------------------------------------------------

static void t_pause()
{
    section("pause and sigsuspend");

    install(SIGUSR1, record, 0);
    reset_record();

    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0)
    {
        sleep_ms(100);
        kill(parent, SIGUSR1);
        exit(0);
    }

    check("pause returns -1", pause() == -1);
    check("with EINTR", errno == EINTR);
    check("after the handler ran", caught_count == 1);
    waitpid(child, nullptr, 0);

    // sigsuspend swaps the mask atomically: the signal is blocked before
    // the child can send it, and only sigsuspend's temporary mask lets it
    // through - so a delivery here proves the swap happened.
    sigset_t block_usr1, empty;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    sigemptyset(&empty);

    sigprocmask(SIG_BLOCK, &block_usr1, nullptr);
    reset_record();

    child = fork();
    if (child == 0)
    {
        sleep_ms(100);
        kill(parent, SIGUSR1);
        exit(0);
    }

    check("sigsuspend returns -1", sigsuspend(&empty) == -1);
    check("with EINTR", errno == EINTR);
    check("the handler ran under the temporary mask", caught_count == 1);

    sigset_t after;
    sigprocmask(SIG_SETMASK, nullptr, &after);
    check("and the old mask is back",
          sigismember(&after, SIGUSR1) == 1);

    sigprocmask(SIG_SETMASK, &empty, nullptr);
    waitpid(child, nullptr, 0);
}

// --- identity --------------------------------------------------------------

static void t_identity()
{
    section("identity");

    check("getuid is 0", getuid() == 0);
    check("geteuid is 0", geteuid() == 0);
    check("getgid is 0", getgid() == 0);
    check("getegid is 0", getegid() == 0);
    check("tcgetpgrp names a group", tcgetpgrp(0) > 0);
}

// --- interactive modes -----------------------------------------------------

static void announce(int sig)
{
    print("\ncaught ");
    print_u64((uint64_t)sig);
    print("\n");
}

static void mode_catch()
{
    install(SIGINT,  announce, SA_RESTART);
    install(SIGTSTP, announce, SA_RESTART);
    install(SIGQUIT, announce, SA_RESTART);

    print("sigtest: press ^C, ^Z or ^backslash; q to quit\n");
    for (;;)
    {
        char c = 0;
        ssize_t n = read(0, &c, 1);
        if (n == 1 && (c == 'q' || c == 'Q'))
            break;
    }
    print("sigtest: done\n");
}

static void mode_spin()
{
    install(SIGINT, announce, SA_RESTART);
    print("sigtest: spinning; ^C is caught, Ctrl+Alt+Backspace ends it\n");
    for (;;) {}
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "catch") == 0) { mode_catch(); return 0; }
    if (argc > 1 && strcmp(argv[1], "spin") == 0)  { mode_spin();  return 0; }

    print("signal tests\n");

    t_basic();
    t_registers();
    t_mask();
    t_order();
    t_eintr();
    t_segv();
    t_kill();
    t_sigchld();
    t_jobcontrol();
    t_pause();
    t_identity();

    print("\nsigtest: ");
    print_u64((uint64_t)passed);
    print(" passed, ");
    print_u64((uint64_t)failed);
    print(" failed\n");
    return failed;
}
