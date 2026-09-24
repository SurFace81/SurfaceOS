// argtest: checks the SysV ABI initial process stack (stage 3.1).
//
//   argtest             print argc, argv, envp and the auxv entries
//   argtest many N      exec itself with N arguments (spawn mode)
//   argtest big         exec itself with one argument > ARG_MAX (must fail)
//   argtest child ...   target of the two exec modes above
//
// Exit status = number of failed checks (0 in spawn/child modes).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <errno.h>
#include <abi/syscall.h>
#include <abi/auxv.h>
#include <abi/process.h>

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    print(ok ? "  [ ok ] " : "  [FAIL] ");
    print(name);
    print("\n");
    if (ok) passed++; else failed++;
}

// The kernel puts auxv right after envp on the initial stack; recover it
// the way libc does: scan envp for NULL, walk one more pointer.
static auxv_t* find_auxv(char** envp)
{
    char** p = envp;
    while (*p)
        p++;
    return (auxv_t*)(p + 1);
}

static uint64_t aux_get(auxv_t* av, uint64_t type, bool* found)
{
    for (; av->a_type != AT_NULL; av++)
        if (av->a_type == type)
        {
            *found = true;
            return av->a_val;
        }
    *found = false;
    return 0;
}

static void print_auxv(auxv_t* av)
{
    print("\nauxv:\n");
    for (; av->a_type != AT_NULL; av++)
    {
        print("  AT_");
        switch (av->a_type)
        {
            case AT_PHDR:   print("PHDR   = "); break;
            case AT_PHENT:  print("PHENT  = "); break;
            case AT_PHNUM:  print("PHNUM  = "); break;
            case AT_PAGESZ: print("PAGESZ = "); break;
            case AT_ENTRY:  print("ENTRY  = "); break;
            case AT_RANDOM: print("RANDOM = "); break;
            default:        print("type "); print_u64(av->a_type);
                            print("  = ");                     break;
        }
        print_hex64(av->a_val);
        print("\n");
    }
}

static void dump(int argc, char** argv)
{
    print("argc = ");
    print_i64(argc);
    print("\nargv:\n");
    for (int i = 0; i < argc; i++)
    {
        print("  [");
        print_i64(i);
        print("] ");
        print(argv[i]);
        print("\n");
    }

    print("envp:\n");
    for (char** e = environ; *e; e++)
    {
        print("  ");
        print(*e);
        print("\n");
    }

    print_auxv(find_auxv(environ));
}

// --- modes -------------------------------------------------------------------

// argtest child <N> <filler>...: verify argv survived the exec round trip.
// argv[0] is the program name, argv[1] == "child", argv[2] == "<N>".
static int child_mode(int argc, char** argv)
{
    bool ok = argc >= 3;
    int want = 0;
    if (ok)
    {
        for (const char* s = argv[2]; *s; s++)
            want = want * 10 + (*s - '0');
        ok = (argc - 3 == want);
    }
    // Every argument after the header must be the expected filler string.
    for (int i = 3; ok && i < argc; i++)
        ok = strcmp(argv[i], "xxxxxxxxxxxxxxxx") == 0;

    print(ok ? "child: argv intact\n" : "child: argv CORRUPT\n");
    exit(ok ? 0 : 1);
    return 0;   // unreachable
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0)
        return child_mode(argc, argv);

    if (argc >= 2 && strcmp(argv[1], "many") == 0)
    {
        const int N = 1000;
        // argv: [self, "child", "<N>", N x filler, NULL]
        const char** av = (const char**)malloc((uint64_t)(N + 4) * 8);
        if (!av)
            exit(2);

        av[0] = argv[0];
        av[1] = "child";
        av[2] = "1000";
        for (int i = 0; i < N; i++)
            av[3 + i] = "xxxxxxxxxxxxxxxx";
        av[N + 3] = NULL;

        print("exec'ing self with 1000 arguments...\n");
        execv(argv[0], (char* const*)av);
        print("execve of 1000 args FAILED unexpectedly (errno ");
        print_i64(errno);
        print(")\n");
        exit(3);
    }

    if (argc >= 2 && strcmp(argv[1], "big") == 0)
    {
        // One argument larger than ARG_MAX: execve must fail with E2BIG and
        // the old program must keep running.
        const uint64_t BIG = 200 * 1024;
        char* huge = (char*)malloc(BIG);
        if (!huge)
            exit(2);
        for (uint64_t i = 0; i < BIG - 1; i++)
            huge[i] = 'y';
        huge[BIG - 1] = '\0';

        const char* av[3] = { argv[0], huge, NULL };
        int r = execv(argv[0], (char* const*)av);
        check("execve with a 200 KiB argument fails", r == -1);
        check("... with E2BIG", errno == E2BIG);
        free(huge);
        return failed;
    }

    // Default: dump everything, then run the two exec modes as children.
    dump(argc, argv);

    print("\nargument passing:\n");

    pid_t many = fork();
    if (many == 0)
    {
        const char* av[] = { argv[0], "many", NULL };
        execv(argv[0], (char* const*)av);
        exit(99);
    }
    int st = -1;
    check("1000 arguments survive execve",
          waitpid(many, &st, 0) == many && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pid_t big = fork();
    if (big == 0)
    {
        const char* av[] = { argv[0], "big", NULL };
        execv(argv[0], (char* const*)av);
        exit(99);
    }
    st = -1;
    check("over-ARG_MAX execve returns -E2BIG",
          waitpid(big, &st, 0) == big && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    print("\nstack layout:\n");
    auxv_t* av = find_auxv(environ);
    bool f = false;

    check("AT_PAGESZ is 4096", aux_get(av, AT_PAGESZ, &f) == 4096 && f);
    uint64_t phnum = aux_get(av, AT_PHNUM, &f);
    check("AT_PHNUM present and sane", f && phnum >= 3 && phnum <= 16);
    uint64_t phent = aux_get(av, AT_PHENT, &f);
    check("AT_PHENT is 56", f && phent == 56);
    uint64_t phdr = aux_get(av, AT_PHDR, &f);
    check("AT_PHDR points into the image", f && phdr >= 0x400000ULL && phdr < 0x1000000ULL);
    uint64_t rnd = aux_get(av, AT_RANDOM, &f);
    bool rnd_ok = f && rnd >= 0x7FFFFFE00000ULL && rnd < 0x800000000000ULL;   // stack
    check("AT_RANDOM points to 16 bytes on the stack", rnd_ok);
    if (rnd_ok)
    {
        // The bytes must actually be readable.
        volatile uint8_t b = ((uint8_t*)rnd)[0] ^ ((uint8_t*)rnd)[15];
        (void)b;
        check("AT_RANDOM bytes are readable", true);
    }

    check("environ is set and PATH is there", getenv("PATH") != NULL);
    check("getenv of a missing name returns NULL", getenv("NO_SUCH_VAR_XYZ") == NULL);
    const char* term = getenv("TERM");
    check("TERM is inherited from the console", term != NULL && strcmp(term, "dumb") == 0);

    print("\nargtest: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");
    return failed;
}
