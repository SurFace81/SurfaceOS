// proctest: exercises processes and the scheduler.
//
//   proctest            run the test suite (exit status = failed checks)
//   proctest spin       loop forever without making syscalls (try Esc)
//   proctest child ...  used by the exec test: prints its argv, exits with 7

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <abi/sfcall.h>

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

static uint64_t now_ms()
{
    uptime_t u;
    get_uptime(&u);
    return u.total_ms;
}

// Deterministic floating-point workload. Long enough to be preempted many
// times, so a scheduler that does not save XMM state corrupts the result.
static double fpu_work(double seed)
{
    volatile double x = seed;
    for (int i = 0; i < 3000000; i++)
        x = x * 1.0000001 + seed * 0.5 - (double)(i & 7) * 0.125;
    return x;
}

static void test_basics()
{
    section("identity");
    pid_t me = getpid();
    print("  pid ");
    print_i64(me);
    print(", parent ");
    print_i64(getppid());
    print("\n");
    check("getpid is positive", me > 0);
    check("started by the console (ppid 0)", getppid() == 0);
}

static void test_fork_wait()
{
    section("fork / exit / waitpid");

    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0)
    {
        if (getppid() != parent)
            exit(1);
        exit(42);
    }

    int status = -1;
    check("fork returns the child's pid", child > 0 && child != parent);
    check("waitpid(child) returns it", waitpid(child, &status, 0) == child);
    check("exit status 42 delivered (and child saw its parent)",
          WIFEXITED(status) && WEXITSTATUS(status) == 42);
    check("waiting again fails: already reaped", waitpid(child, &status, 0) == -1);
    check("waitpid with no children fails", waitpid(-1, &status, WNOHANG) == -1);

    const int N = 5;
    for (int i = 0; i < N; i++)
    {
        if (fork() == 0)
        {
            sleep_ms((uint32_t)(10 * (N - i)));   // finish in reverse order
            exit(i + 1);
        }
    }

    int sum = 0, reaped = 0;
    for (int i = 0; i < N; i++)
    {
        int s = 0;
        if (waitpid(-1, &s, 0) > 0)
        {
            sum += WEXITSTATUS(s);
            reaped++;
        }
    }
    check("five children reaped with waitpid(-1)", reaped == N);
    check("their statuses add up (1+2+3+4+5)", sum == 15);
}

static void test_nohang_sleep()
{
    section("sleep / WNOHANG");

    uint64_t t0 = now_ms();
    sleep_ms(200);
    uint64_t dt = now_ms() - t0;
    check("sleep_ms(200) sleeps at least ~200 ms", dt >= 190);
    check("... and not wildly longer", dt < 1000);

    pid_t child = fork();
    if (child == 0)
    {
        sleep_ms(150);
        exit(3);
    }

    int status = -1;
    check("WNOHANG on a running child returns 0", waitpid(child, &status, WNOHANG) == 0);
    check("blocking waitpid then collects it",
          waitpid(child, &status, 0) == child && WEXITSTATUS(status) == 3);
}

static void test_preemption()
{
    section("preemption");

    // The child never makes a syscall. Without timer preemption the parent
    // would not run again until the machine is reset.
    pid_t spinner = fork();
    if (spinner == 0)
        for (;;) {}

    uint64_t t0 = now_ms();
    sleep_ms(100);
    check("parent keeps running next to a busy-looping child", now_ms() - t0 >= 90);

    check("kill(spinner, SIGKILL)", kill(spinner, SIGKILL) == 0);
    int status = -1;
    check("killed child reports SIGKILL",
          waitpid(spinner, &status, 0) == spinner &&
          WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    check("kill of a dead pid fails", kill(spinner, SIGKILL) == -1);
}

static void test_fpu()
{
    section("FPU/SSE state per process");

    double expect_a = fpu_work(1.5);
    double expect_b = fpu_work(-2.25);

    pid_t child = fork();
    if (child == 0)
        exit(fpu_work(-2.25) == expect_b ? 0 : 1);

    double a = fpu_work(1.5);
    int status = -1;
    waitpid(child, &status, 0);

    check("parent result unaffected by the child", a == expect_a);
    check("child result unaffected by the parent", WEXITSTATUS(status) == 0);
}

static void test_exec(const char* self)
{
    section("exec");

    pid_t child = fork();
    if (child == 0)
    {
        char a0[32], a1[] = "child", a2[] = "hello world";
        int i = 0;
        for (; self[i] && i < 31; i++) a0[i] = self[i];
        a0[i] = '\0';

        char* argv[] = { a0, a1, a2, NULL };
        execv(self, argv);
        exit(99);           // exec failed
    }

    int status = -1;
    check("exec'd program ran with our argv (status 7)",
          waitpid(child, &status, 0) == child && WEXITSTATUS(status) == 7);

    char* argv[] = { NULL };
    check("exec of a missing file fails and returns", execv("NOSUCH", argv) == -1);
    check("still running after the failed exec", getpid() > 0);
}

// --- the SurfaceOS ABI entry (the `syscall` instruction) -------------------

// One call with marker values in every register that must survive it.
struct sf_probe
{
    uint64_t nr;
    uint64_t status;
    uint64_t in[11];    // rbx r12 r13 r14 r15 rdi rsi rdx r8 r9 r10
    uint64_t out[11];
};

static void sf_call_probe(sf_probe* p)
{
    // 128 bytes down first: this function may keep locals in the red zone,
    // and the pushes below would land on them.
    asm volatile(
        "sub $128, %%rsp\n"
        "push %%rbx\n push %%rbp\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
        "push %0\n"
        "mov %0, %%rax\n"
        "mov 16(%%rax), %%rbx\n mov 24(%%rax), %%r12\n mov 32(%%rax), %%r13\n"
        "mov 40(%%rax), %%r14\n mov 48(%%rax), %%r15\n mov 56(%%rax), %%rdi\n"
        "mov 64(%%rax), %%rsi\n mov 72(%%rax), %%rdx\n mov 80(%%rax), %%r8\n"
        "mov 88(%%rax), %%r9\n mov 96(%%rax), %%r10\n"
        "mov 0(%%rax), %%rax\n"
        "syscall\n"
        "pop %%rcx\n"
        "mov %%rax, 8(%%rcx)\n"
        "mov %%rbx, 104(%%rcx)\n mov %%r12, 112(%%rcx)\n mov %%r13, 120(%%rcx)\n"
        "mov %%r14, 128(%%rcx)\n mov %%r15, 136(%%rcx)\n mov %%rdi, 144(%%rcx)\n"
        "mov %%rsi, 152(%%rcx)\n mov %%rdx, 160(%%rcx)\n mov %%r8, 168(%%rcx)\n"
        "mov %%r9, 176(%%rcx)\n mov %%r10, 184(%%rcx)\n"
        "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbp\n pop %%rbx\n"
        "add $128, %%rsp\n"
        :: "r"(p)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
}

static bool sf_regs_survive(uint64_t nr, uint64_t* status)
{
    sf_probe p;
    p.nr = nr;
    for (int i = 0; i < 11; i++)
    {
        p.in[i] = 0x5F00000000000000ULL + (uint64_t)i * 0x0101010101ULL + nr;
        p.out[i] = 0;
    }
    sf_call_probe(&p);
    *status = p.status;
    for (int i = 0; i < 11; i++)
        if (p.out[i] != p.in[i])
            return false;
    return true;
}

static void test_sfcall()
{
    section("syscall instruction (SurfaceOS ABI)");

    uint64_t st = 0;
    bool kept = sf_regs_survive(0, &st);
    check("an unknown call returns SF_UNSUPPORTED", st == SF_UNSUPPORTED);
    check("every register but rax/rcx/r11 survives", kept);

    kept = sf_regs_survive(0xFFFFFFFFULL, &st);
    check("a huge call number is SF_UNSUPPORTED too", st == SF_UNSUPPORTED && kept);

    // Two processes calling back to back: the timer switches between them
    // in the middle of calls, and each must come back on its own kernel
    // stack with its own registers.
    pid_t child = fork();
    uint64_t t0 = now_ms();
    bool all = true;
    int n = 0;
    while (now_ms() - t0 < 300)
    {
        if (!sf_regs_survive((uint64_t)n & 7, &st) || st != SF_UNSUPPORTED)
            all = false;
        n++;
    }
    if (child == 0)
        exit(all && n > 100 ? 0 : 1);

    int status = -1;
    waitpid(child, &status, 0);
    check("300 ms of calls from two processes at once come back intact",
          all && n > 100 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "spin") == 0)
    {
        print("spinning without syscalls - press Esc\n");
        for (;;) {}
    }

    if (argc >= 2 && strcmp(argv[1], "child") == 0)
    {
        print("  exec child: argc=");
        print_i64(argc);
        for (int i = 0; i < argc; i++)
        {
            print(" [");
            print(argv[i]);
            print("]");
        }
        print("\n");
        return (argc == 3 && strcmp(argv[2], "hello world") == 0) ? 7 : 8;
    }

    print("proctest - Esc terminates\n");

    test_basics();
    test_fork_wait();
    test_nohang_sleep();
    test_preemption();
    test_fpu();
    test_sfcall();
    test_exec(argc >= 1 ? argv[0] : "proctest");

    print("\nproctest: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");

    print("Press any key to return to the console\n");
    read_key();

    return failed;
}
