// memtest: exercises the memory side of the kernel from user space.
//
//   brk        grow, write, shrink, out-of-range requests
//   malloc     many mixed-size blocks, pattern checks, free/reuse, 8 MiB block
//   mmap       large anonymous mappings, hints, invalid arguments
//   mprotect   read-only pages, PROT_NONE, W->X for generated code (JIT)
//   isolation  fork copies memory instead of sharing it
//   uaccess    kernel pointers passed to syscalls are rejected, not written
//   faults     every protection violation kills only the offending child
//
// Exit status = number of failed checks. Esc terminates it at any time.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mman.h>
#include <syscall.h>
#include <errno.h>
#include <abi/syscall.h>

static const uint64_t PAGE = 4096;
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

static uint8_t pattern(uint64_t block, uint64_t offset)
{
    return (uint8_t)(block * 31 + offset * 7 + 1);
}

// ---------------------------------------------------------------------------

static void test_brk()
{
    section("brk");

    uint64_t base = brk(0);
    check("brk(0) reports a break", base != 0);

    uint64_t want = base + 3 * PAGE + 123;
    check("grow by 3 pages", brk(want) == want);

    bool ok = true;
    for (uint64_t a = base; a < want; a++)
        ((uint8_t*)a)[0] = (uint8_t)a;
    for (uint64_t a = base; a < want; a++)
        if (((uint8_t*)a)[0] != (uint8_t)a) ok = false;
    check("grown pages are writable and keep data", ok);

    check("shrink back", brk(base) == base);
    check("break below the image is refused", brk(0x1000) == base);
    check("absurd break is refused", brk(0x7FFFFFFFFFFFULL) == base);
}

static void test_malloc()
{
    section("malloc / free");

    const int N = 200;
    uint8_t* blocks[N];
    uint64_t sizes[N];
    bool all = true;

    for (int i = 0; i < N; i++)
    {
        sizes[i] = 1 + (uint64_t)((i * 7919) % 20000);
        blocks[i] = (uint8_t*)malloc(sizes[i]);
        if (!blocks[i]) { all = false; break; }
        for (uint64_t j = 0; j < sizes[i]; j++)
            blocks[i][j] = pattern((uint64_t)i, j);
    }
    check("200 blocks of 1..20000 bytes", all);

    bool intact = all;
    for (int i = 0; all && i < N; i++)
        for (uint64_t j = 0; j < sizes[i]; j++)
            if (blocks[i][j] != pattern((uint64_t)i, j)) { intact = false; break; }
    check("every byte survives neighbouring writes", intact);

    for (int i = 1; all && i < N; i += 2)
        free(blocks[i]);

    bool refill = all;
    for (int i = 1; all && i < N; i += 2)
    {
        blocks[i] = (uint8_t*)malloc(sizes[i]);
        if (!blocks[i]) { refill = false; break; }
        for (uint64_t j = 0; j < sizes[i]; j++)
            blocks[i][j] = (uint8_t)~pattern((uint64_t)i, j);
    }
    check("freed half re-allocated", refill);

    bool evens = all;
    for (int i = 0; all && i < N; i += 2)
        for (uint64_t j = 0; j < sizes[i]; j++)
            if (blocks[i][j] != pattern((uint64_t)i, j)) { evens = false; break; }
    check("untouched blocks unchanged after reuse", evens);

    for (int i = 0; all && i < N; i++)
        free(blocks[i]);

    const uint64_t BIG = 8 * 1024 * 1024;
    uint8_t* big = (uint8_t*)malloc(BIG);
    bool big_ok = big != NULL;
    if (big_ok)
    {
        for (uint64_t j = 0; j < BIG; j += PAGE)
            big[j] = (uint8_t)(j >> 12);
        big[BIG - 1] = 0xA5;
        for (uint64_t j = 0; j < BIG; j += PAGE)
            if (big[j] != (uint8_t)(j >> 12)) big_ok = false;
        big_ok = big_ok && big[BIG - 1] == 0xA5;
        free(big);
    }
    check("8 MiB block, every page touched", big_ok);

    check("malloc(0) returns NULL", malloc(0) == NULL);
}

static void test_mmap()
{
    section("mmap / munmap");

    const uint64_t LEN = 16 * 1024 * 1024;
    uint8_t* m = (uint8_t*)mmap(NULL, LEN, PROT_READ | PROT_WRITE);
    check("map 16 MiB read/write", m != MAP_FAILED);

    if (m != MAP_FAILED)
    {
        bool zero = true;
        for (uint64_t o = 0; o < LEN; o += PAGE)
            if (m[o] != 0) zero = false;
        check("fresh mapping is zero-filled", zero);

        for (uint64_t o = 0; o < LEN; o += PAGE / 2)
            m[o] = (uint8_t)(o >> 11);
        bool ok = true;
        for (uint64_t o = 0; o < LEN; o += PAGE / 2)
            if (m[o] != (uint8_t)(o >> 11)) ok = false;
        check("write/verify across all pages", ok);

        check("munmap", munmap(m, LEN) == 0);
    }

    void* a = mmap(NULL, PAGE, PROT_READ | PROT_WRITE);
    void* b = mmap(a, PAGE, PROT_READ | PROT_WRITE);
    check("hint on an occupied range picks another address",
          a != MAP_FAILED && b != MAP_FAILED && a != b);
    if (b != MAP_FAILED) munmap(b, PAGE);
    if (a != MAP_FAILED)
    {
        munmap(a, PAGE);
        void* c = mmap(a, PAGE, PROT_READ | PROT_WRITE);
        check("hint on a free range is honoured", c == a);
        if (c != MAP_FAILED) munmap(c, PAGE);
    }

    check("length 0 is refused", mmap(NULL, 0, PROT_READ) == MAP_FAILED);
    check("unknown prot bits are refused", mmap(NULL, PAGE, 0x80) == MAP_FAILED);
    check("munmap of a misaligned address is refused", munmap((void*)0x40010000001ULL, PAGE) != 0);
    check("munmap outside the mmap region is refused", munmap((void*)0x40000100000ULL, PAGE) != 0);
}

static void test_jit()
{
    section("mprotect / generated code");

    uint8_t* code = (uint8_t*)mmap(NULL, PAGE, PROT_READ | PROT_WRITE);
    check("map a page for code", code != MAP_FAILED);
    if (code == MAP_FAILED)
        return;

    // mov eax, 42 ; ret
    static const uint8_t prog[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    for (uint64_t i = 0; i < sizeof(prog); i++)
        code[i] = prog[i];

    check("mprotect RW -> RX", mprotect(code, PAGE, PROT_READ | PROT_EXEC) == 0);
    int (*fn)() = (int (*)())code;
    check("generated function returns 42", fn() == 42);

    check("mprotect of an unmapped range is refused",
          mprotect((void*)((uint64_t)code + 64 * PAGE), PAGE, PROT_READ) != 0);
    check("mprotect of the argv page is refused",
          mprotect((void*)0x40030001000ULL, PAGE, PROT_READ | PROT_WRITE) != 0);

    munmap(code, PAGE);

    uint8_t* rwx = (uint8_t*)mmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
    bool ok = rwx != MAP_FAILED;
    if (ok)
    {
        // mov eax, 7 ; ret
        rwx[0] = 0xB8; rwx[1] = 7; rwx[2] = 0; rwx[3] = 0; rwx[4] = 0; rwx[5] = 0xC3;
        ok = ((int (*)())rwx)() == 7;
        munmap(rwx, PAGE);
    }
    check("RWX mapping (tcc -run style)", ok);
}

static void test_isolation()
{
    section("fork memory isolation");

    static uint64_t shared_looking = 1;
    uint8_t* heap = (uint8_t*)malloc(64);
    heap[0] = 11;

    pid_t child = fork();
    if (child == 0)
    {
        shared_looking = 2;
        heap[0] = 22;
        exit((int)(shared_looking * 10 + heap[0] / 11));   // 22
    }

    int status = -1;
    check("fork", child > 0);
    check("waitpid returns the child", waitpid(child, &status, 0) == child);
    check("child saw its own writes", WIFEXITED(status) && WEXITSTATUS(status) == 22);
    check("parent memory untouched", shared_looking == 1 && heap[0] == 11);
    free(heap);
}

static void test_uaccess()
{
    section("syscall pointer validation");

    // Addresses owned by the kernel: its image, the PMM bitmap, the page
    // tables. Before uaccess existed each of these calls wrote there at CPL 0.
    const uint64_t kernel_image  = 0x200000;
    const uint64_t pmm_bitmap    = 0x3000000;
    const uint64_t page_tables   = 0x300000;

    check("SYS_TIME into the PMM bitmap is refused",
          syscall(SYSX_TIME, pmm_bitmap) == -EFAULT);
    check("SYS_UPTIME into the page tables is refused",
          syscall(SYSX_UPTIME, page_tables) == -EFAULT);
    check("SYS_READ_KEY into the kernel image is refused",
          syscall(SYSX_READ_KEY, kernel_image) == -EFAULT);
    check("write(1, kernel string) is refused",
          syscall(SYS_WRITE, 1, kernel_image, 16) == -EFAULT);
    check("SYS_STAT_FILE with a kernel path is refused",
          syscall(SYSX_STAT_FILE, kernel_image, pmm_bitmap) == -EFAULT);
    check("SYS_WAIT4 status into the kernel is refused",
          syscall(SYS_WAIT4, (uint64_t)-1, pmm_bitmap, WNOHANG) == -EFAULT);
    check("unknown syscall returns -ENOSYS",
          syscall(9999) == -ENOSYS);

    static const char rodata[] = "read-only";
    datetime_t* ro = (datetime_t*)(uint64_t)rodata;
    check("SYS_TIME into our own .rodata is refused",
          syscall(SYSX_TIME, (uint64_t)ro) == -EFAULT);

    datetime_t ok;
    check("SYS_TIME into a valid buffer still works",
          syscall(SYSX_TIME, (uint64_t)&ok) == 0);
}

// ---------------------------------------------------------------------------
// Faults: each case runs in a child that must be killed by the CPU exception,
// while this process carries on.

static const char rodata_string[] = "constant";

static int fault_rodata()
{
    ((char*)(uint64_t)rodata_string)[0] = 'X';
    return 0;
}

static int fault_text()
{
    uint8_t* self = (uint8_t*)(uint64_t)&fault_text;
    self[0] = 0xC3;
    return 0;
}

static int fault_stack_exec()
{
    uint8_t code[8] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    return ((int (*)())(uint64_t)code)();
}

static int fault_heap_exec()
{
    uint8_t* code = (uint8_t*)malloc(16);
    code[0] = 0xC3;
    ((void (*)())(uint64_t)code)();
    return 0;
}

static int fault_readonly_after_mprotect()
{
    uint8_t* p = (uint8_t*)mmap(NULL, PAGE, PROT_READ | PROT_WRITE);
    p[0] = 1;
    mprotect(p, PAGE, PROT_READ);
    p[0] = 2;
    return 0;
}

static int fault_prot_none()
{
    uint8_t* p = (uint8_t*)mmap(NULL, PAGE, PROT_NONE);
    volatile uint8_t v = p[0];
    return v;
}

static int fault_kernel_read()
{
    volatile uint8_t v = ((uint8_t*)0x200000)[0];
    return v;
}

static int fault_null()
{
    volatile uint8_t v = ((uint8_t*)(uint64_t)0)[0];
    return v;
}

static int fault_unmapped_after_munmap()
{
    uint8_t* p = (uint8_t*)mmap(NULL, PAGE, PROT_READ | PROT_WRITE);
    munmap(p, PAGE);
    p[0] = 1;
    return 0;
}

static int fault_divide()
{
    volatile int zero = 0;
    volatile int r = 10 / zero;
    return r;
}

static int fault_privileged()
{
    asm volatile("cli");
    return 0;
}

static void expect_fault(const char* name, int (*fn)(), int expected_signal)
{
    pid_t child = fork();
    if (child == 0)
        exit(fn() + 1);         // reaching this line means no fault

    int status = -1;
    bool ok = child > 0 && waitpid(child, &status, 0) == child &&
              WIFSIGNALED(status) && WTERMSIG(status) == expected_signal;
    check(name, ok);
    if (!ok)
    {
        print("         signal ");
        print_i64(WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        print(", expected ");
        print_i64(expected_signal);
        print("\n");
    }
}

static void test_faults()
{
    section("protection faults (each in a child)");

    expect_fault("write to .rodata",                fault_rodata,                SIGSEGV);
    expect_fault("write to .text",                  fault_text,                  SIGSEGV);
    expect_fault("execute on the stack (NX)",       fault_stack_exec,            SIGSEGV);
    expect_fault("execute on the heap (NX)",        fault_heap_exec,             SIGSEGV);
    expect_fault("write after mprotect(PROT_READ)", fault_readonly_after_mprotect, SIGSEGV);
    expect_fault("read PROT_NONE page",             fault_prot_none,             SIGSEGV);
    expect_fault("read kernel memory",              fault_kernel_read,           SIGSEGV);
    expect_fault("NULL dereference",                fault_null,                  SIGSEGV);
    expect_fault("use after munmap",                fault_unmapped_after_munmap, SIGSEGV);
    expect_fault("divide by zero",                  fault_divide,                SIGFPE);
    expect_fault("privileged instruction (cli)",    fault_privileged,            SIGSEGV);
}

// ---------------------------------------------------------------------------

int main()
{
    print("memtest (pid ");
    print_i64(getpid());
    print(") - Esc terminates\n");

    test_brk();          // first: before malloc starts moving the break
    test_malloc();
    test_mmap();
    test_jit();
    test_isolation();
    test_uaccess();
    test_faults();

    print("\nmemtest: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");

    // The console clears the screen when the app ends; keep the report up.
    print("Press any key to return to the console\n");
    read_key();

    return failed;
}
