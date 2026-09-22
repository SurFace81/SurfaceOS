// proctest: exercises processes and the scheduler.
//
//   proctest            run the test suite (exit status = failed checks)
//   proctest spin       loop forever without making syscalls (try Esc)
//   proctest child ...  used by the exec test: prints its argv, exits with 7

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
