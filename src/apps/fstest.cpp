// fstest: exercises the fd layer, the VFS and the FAT32 driver (stage 3).
//
//   fstest          run the suite against /test/fstest.d (created fresh)
//   fstest verify   read back what the previous run left (persistence after
//                   a QEMU restart with the same image)
//
// Every check prints [ ok ]/[FAIL]; exit status = number of failures.
// Writes go through fsync where persistence matters.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <syscall.h>
#include <sys/stat.h>
#include <abi/syscall.h>
#include <abi/stat.h>
#include <abi/fcntl.h>
#include <abi/termios.h>

#define PATH_MAX_TEST 4096

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

static const char* D = "/test/fstest.d";

static uint8_t byte_of(uint32_t i)
{
    return (uint8_t)((i * 7919 + 13) & 0xFF);
}

// Deterministic fill so read-back checks are exact.
static void fill(uint8_t* b, uint32_t base, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        b[i] = byte_of(base + i);
}

static bool same(const uint8_t* b, uint32_t base, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (b[i] != byte_of(base + i))
            return false;
    return true;
}

// ===========================================================================

static void test_basic_rw()
{
    section("open / write / read");

    char path[256];
    strcpy(path, D);
    strcat(path, "/basic.txt");

    const char* msg = "hello, surfaceos\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check("open(O_WRONLY|O_CREAT|O_TRUNC) returns a small fd", fd >= 3);
    ssize_t w = write(fd, msg, strlen(msg));
    check("write returns the count", w == (ssize_t)strlen(msg));
    check("close", close(fd) == 0);

    char buf[64];
    fd = open(path, O_RDONLY);
    check("reopen O_RDONLY", fd >= 3);
    ssize_t r = read(fd, buf, sizeof(buf));
    check("read returns the whole content", r == (ssize_t)strlen(msg));
    buf[r > 0 ? r : 0] = '\0';
    check("content matches", strcmp(buf, msg) == 0);
    r = read(fd, buf, sizeof(buf));
    check("read at EOF returns 0", r == 0);
    close(fd);
}

static void test_big_file()
{
    section("3 MiB file, odd block sizes, random lseek");

    char path[256];
    strcpy(path, D);
    strcat(path, "/big.bin");

    const uint32_t SIZE = 3 * 1024 * 1024;
    const uint32_t WBLK = 4097;
    const uint32_t RBLK = 1000;

    uint8_t* buf = (uint8_t*)malloc(WBLK > RBLK ? WBLK : RBLK);
    if (!buf)
    {
        check("alloc buffer", false);
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check("create big.bin", fd >= 3);

    uint32_t off = 0;
    bool write_ok = true;
    while (off < SIZE)
    {
        uint32_t n = SIZE - off;
        if (n > WBLK) n = WBLK;
        fill(buf, off, n);
        ssize_t w = write(fd, buf, n);
        if (w != (ssize_t)n) { write_ok = false; break; }
        off += n;
    }
    check("3 MiB written in 4097-byte blocks", write_ok);
    check("fsync", fsync(fd) == 0);
    close(fd);

    struct stat st;
    check("stat size is 3 MiB", stat(path, &st) == 0 && st.st_size == SIZE);

    // Read back in 1000-byte blocks at deterministic pseudo-random offsets.
    fd = open(path, O_RDONLY);
    check("reopen big.bin", fd >= 3);

    bool read_ok = true;
    uint32_t pos = 0;
    uint32_t seed = 12345;
    for (int i = 0; i < 40 && read_ok; i++)
    {
        seed = seed * 1103515245 + 12345;
        pos = (seed >> 7) % (SIZE - RBLK);
        off_t got = lseek(fd, pos, SEEK_SET);
        if (got != (off_t)pos) { read_ok = false; break; }

        ssize_t r = read(fd, buf, RBLK);
        if (r != (ssize_t)RBLK) { read_ok = false; break; }
        if (!same(buf, pos, RBLK)) { read_ok = false; break; }
    }
    check("40 random-lseek 1000-byte reads match", read_ok);

    // Sequential read of the whole file through a short-buffer loop.
    lseek(fd, 0, SEEK_SET);
    uint32_t total = 0;
    bool seq_ok = true;
    while (total < SIZE)
    {
        uint32_t n = SIZE - total;
        if (n > RBLK) n = RBLK;
        ssize_t r = read(fd, buf, n);
        if (r <= 0) { seq_ok = false; break; }
        if (!same(buf, total, (uint32_t)r)) { seq_ok = false; break; }
        total += (uint32_t)r;
    }
    check("sequential read of all 3 MiB matches", seq_ok && total == SIZE);
    close(fd);
    free(buf);
}

static void test_flags()
{
    section("O_APPEND / O_TRUNC / O_EXCL / ftruncate / holes");

    char path[256];
    strcpy(path, D);
    strcat(path, "/flags.txt");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "AAAA", 4);
    close(fd);

    // O_APPEND always lands at the end.
    fd = open(path, O_WRONLY | O_APPEND);
    lseek(fd, 0, SEEK_SET);             // ignored for O_APPEND writes
    write(fd, "BB", 2);
    close(fd);

    struct stat st;
    stat(path, &st);
    check("O_APPEND writes at the end (size 6)", st.st_size == 6);

    fd = open(path, O_RDONLY);
    char buf[16];
    read(fd, buf, 6);
    close(fd);
    buf[6] = '\0';
    check("content is AAAABB", strcmp(buf, "AAAABB") == 0);

    // O_EXCL on an existing file: -EEXIST.
    errno = 0;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    check("O_CREAT|O_EXCL on existing file fails", fd == -1);
    check("... with EEXIST", errno == EEXIST);

    // O_TRUNC resets to zero.
    fd = open(path, O_WRONLY | O_TRUNC);
    fstat(fd, &st);
    check("O_TRUNC empties the file", st.st_size == 0);
    close(fd);

    // ftruncate: shrink then grow (grown tail reads as zeroes).
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    write(fd, "0123456789", 10);
    check("ftruncate shrink to 4", ftruncate(fd, 4) == 0);
    fstat(fd, &st);
    check("size is 4 after shrink", st.st_size == 4);
    check("ftruncate grow to 16", ftruncate(fd, 16) == 0);
    fstat(fd, &st);
    check("size is 16 after grow", st.st_size == 16);
    close(fd);

    fd = open(path, O_RDONLY);
    ssize_t r = read(fd, buf, 16);
    close(fd);
    check("grow tail is zero-filled",
          r == 16 && buf[0] == '0' && buf[3] == '3' && buf[4] == 0 && buf[15] == 0);

    // Write past EOF: the hole reads as zeroes.
    strcpy(path, D);
    strcat(path, "/hole.bin");
    unlink(path);
    fd = open(path, O_RDWR | O_CREAT, 0644);
    lseek(fd, 100, SEEK_SET);
    write(fd, "END", 3);
    lseek(fd, 0, SEEK_SET);
    r = read(fd, buf, 103);
    bool hole_ok = r == 103 && buf[99] == 0 && buf[0] == 0 &&
                   buf[100] == 'E' && buf[102] == 'D';
    check("write past EOF: hole reads as zeroes", hole_ok);
    close(fd);
}

static void test_offsets()
{
    section("pread/pwrite offsets, dup/fork sharing, dup2, O_CLOEXEC");

    char path[256];
    strcpy(path, D);
    strcat(path, "/off.bin");

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    pwrite(fd, "XXXXXXXX", 8, 0);

    // pwrite must not move the offset.
    check("pwrite does not move the offset", lseek(fd, 0, SEEK_CUR) == 0);
    pwrite(fd, "AB", 2, 4);
    char buf[16];
    ssize_t r = pread(fd, buf, 8, 0);
    check("pread reads what pwrite wrote",
          r == 8 && buf[4] == 'A' && buf[5] == 'B' && buf[6] == 'X');
    check("pread does not move the offset", lseek(fd, 0, SEEK_CUR) == 0);

    // dup shares the offset.
    int fd2 = dup(fd);
    check("dup returns a new fd", fd2 >= 3 && fd2 != fd);
    lseek(fd, 3, SEEK_SET);
    check("dup'd fd sees the shared offset", lseek(fd2, 0, SEEK_CUR) == 3);
    write(fd2, "Z", 1);                 // writes at 3, moves both to 4
    check("write through the dup moves both", lseek(fd, 0, SEEK_CUR) == 4);
    close(fd2);

    // fork shares the offset through the copied description.
    pid_t child = fork();
    if (child == 0)
    {
        // Child: move the shared offset, then exit.
        lseek(fd, 8, SEEK_SET);
        exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("fork'd child moved the shared offset",
          WIFEXITED(status) && lseek(fd, 0, SEEK_CUR) == 8);
    close(fd);

    // dup2(fd, 1) redirects stdout.
    strcpy(path, D);
    strcat(path, "/redirect.txt");
    int saved = dup(1);
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(out, 1);
    close(out);
    print("redirected line\n");
    dup2(saved, 1);
    close(saved);

    out = open(path, O_RDONLY);
    r = read(out, buf, 16);
    close(out);
    buf[r > 0 ? r : 0] = '\0';
    check("dup2(fd,1) redirected stdout into a file",
          strcmp(buf, "redirected line\n") == 0);

    // dup3 rejects oldfd == newfd (raw syscall: -errno form). Use fd 0:
    // it is always open.
    check("dup3(fd, fd, O_CLOEXEC) -> EINVAL",
          syscall(SYS_DUP3, 0, 0, (uint64_t)O_CLOEXEC) == -EINVAL);
}

// Regressions for the stage-3 cleanup. Each of these was a real defect:
// F_DUPFD closed whatever sat at minfd, dup2(fd,fd) returned -1, and read()
// blocked even on an O_NONBLOCK fd.
static void test_dup_fcntl_regressions()
{
    section("regressions: F_DUPFD, dup2(fd,fd), O_NONBLOCK");

    char path[256];
    strcpy(path, D);
    strcat(path, "/dupreg.bin");

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open the probe file", fd >= 3);
    write(fd, "hello", 5);

    // dup2 with old == new is a POSIX no-op that still returns the fd.
    check("dup2(fd, fd) returns fd, not -1", dup2(fd, fd) == fd);
    check("... and fd 0 is not a special case", dup2(0, 0) == 0);
    check("... and the fd still works after the no-op",
          lseek(fd, 0, SEEK_SET) == 0);

    // F_DUPFD must find the lowest *free* fd at or above minfd. Park a
    // sentinel at 20 that points at a *different* file, so that a slot
    // stealing implementation is caught: fd 20 would survive as an open fd
    // either way, but it would be pointing at the wrong file.
    char gpath[256];
    strcpy(gpath, D);
    strcat(gpath, "/dupreg.guard");
    int gsrc = open(gpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(gsrc, "abc", 3);              // 3 bytes vs the probe's 5
    int guard = dup2(gsrc, 20);
    close(gsrc);
    check("parked a sentinel fd at 20", guard == 20);

    int got = fcntl(fd, F_DUPFD, 20);
    check("F_DUPFD skipped the occupied slot", got > 20);

    struct stat st;
    errno = 0;
    check("F_DUPFD left the sentinel at 20 pointing at its own file",
          fstat(20, &st) == 0 && st.st_size == 3);
    check("F_DUPFD result is a working dup of fd",
          got >= 0 && lseek(got, 0, SEEK_CUR) == lseek(fd, 0, SEEK_CUR));

    // F_DUPFD_CLOEXEC sets the flag on the new fd, not on the old one.
    int cl = fcntl(fd, F_DUPFD_CLOEXEC, 20);
    check("F_DUPFD_CLOEXEC returns another free fd", cl > 20 && cl != got);
    check("... with FD_CLOEXEC set", fcntl(cl, F_GETFD, 0) == FD_CLOEXEC);
    check("... and the source fd untouched", fcntl(fd, F_GETFD, 0) == 0);

    close(cl);
    close(got);
    close(20);
    close(fd);

    // O_NONBLOCK on the tty: read must report EAGAIN instead of blocking.
    // (If this regresses, the test hangs rather than fails - the harness
    // timeout is the backstop.)
    int tty = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    check("open /dev/tty O_NONBLOCK", tty >= 3);
    check("F_GETFL reports O_NONBLOCK",
          (fcntl(tty, F_GETFL, 0) & O_NONBLOCK) != 0);
    char c = 0;
    errno = 0;
    ssize_t nb = read(tty, &c, 1);
    check("non-blocking read of an idle tty gives EAGAIN",
          nb == -1 && errno == EAGAIN);

    // Clearing it through F_SETFL must be visible too.
    fcntl(tty, F_SETFL, 0);
    check("F_SETFL cleared O_NONBLOCK",
          (fcntl(tty, F_GETFL, 0) & O_NONBLOCK) == 0);
    close(tty);
}

// rename() on FAT changes a file's cache key (it encodes the directory
// slot). The vnode has to be re-keyed, not evicted: evicting let a later
// open of the new path build a second vnode for the same file, with its own
// stale size.
static void test_rename_coherency()
{
    section("rename keeps one vnode per file");

    char oldp[256], newp[256];
    strcpy(oldp, D);
    strcat(oldp, "/coh_old.bin");
    strcpy(newp, D);
    strcat(newp, "/coh_new.bin");

    unlink(newp);
    int a = open(oldp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open the file to be renamed", a >= 3);
    write(a, "12345678", 8);

    check("rename with the fd still open", rename(oldp, newp) == 0);

    // Open the new name: this must land on the *same* vnode as `a`.
    int b = open(newp, O_RDWR);
    check("open the renamed path", b >= 3);

    struct stat sa, sb;
    check("both fds agree on the size after the rename",
          fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_size == 8 &&
          sb.st_size == 8);
    check("both fds report the same inode",
          sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);

    // Grow through the old fd; the fd opened after the rename must see it.
    write(a, "9012", 4);
    check("write through the pre-rename fd is visible to the new fd",
          fstat(b, &sb) == 0 && sb.st_size == 12);

    // ... and the other way round.
    char buf[16];
    ssize_t r = pread(b, buf, 12, 0);
    check("the renamed file reads back intact",
          r == 12 && strncmp(buf, "123456789012", 12) == 0);

    close(a);
    close(b);
    check("the old name is gone", access(oldp, F_OK) == -1);
}

// "fstest cloexec": running inside the exec'd child; fd 7 must be closed,
// fd 8 (no CLOEXEC) must have survived.
static int cloexec_mode()
{
    struct stat st;
    errno = 0;
    bool cloexec_closed = fstat(7, &st) == -1 && errno == EBADF;
    errno = 0;
    bool plain_kept = fstat(8, &st) == 0;
    exit(cloexec_closed && plain_kept ? 0 : 1);
    return 0;
}

static void test_cloexec(const char* self)
{
    section("O_CLOEXEC across execve");

    // A non-CLOEXEC control fd (8) and a CLOEXEC fd (7), both on a real file.
    char path[256];
    strcpy(path, D);
    strcat(path, "/cloexec.src");
    int src = open(path, O_RDONLY | O_CREAT, 0644);
    check("open source file", src >= 3);

    sint64_t r7 = syscall(SYS_DUP3, (uint64_t)src, 7, (uint64_t)O_CLOEXEC);
    sint64_t r8 = syscall(SYS_DUP3, (uint64_t)src, 8, 0);
    close(src);
    check("dup3 set up fd 7 (CLOEXEC) and fd 8 (plain)", r7 == 7 && r8 == 8);

    pid_t child = fork();
    if (child == 0)
    {
        char* argv[] = { (char*)self, (char*)"cloexec", NULL };
        execv(self, argv);
        exit(99);
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("exec'd child ran", WIFEXITED(status));
    check("O_CLOEXEC fd was closed by execve (plain fd kept)",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(7);
    close(8);
}

static void test_lfn()
{
    section("long file names (LFN)");

    char base[256];
    strcpy(base, D);
    strcat(base, "/lfn");
    mkdir(base, 0755);

    const char* names[] =
    {
        "Long File Name.txt",
        "lower.c",
        "UPPER.C",
        "a.b.c.d",
        "mixed Case name.WithLongExtension",
    };
    const int NN = sizeof(names) / sizeof(names[0]);

    for (int i = 0; i < NN; i++)
    {
        char p[512];
        strcpy(p, base);
        strcat(p, "/");
        strcat(p, names[i]);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 3)
        {
            write(fd, "x", 1);
            close(fd);
        }
        check("created an LFN file", fd >= 3);
    }

    // 100 files with long names in one directory (forces directory growth).
    bool many_ok = true;
    for (int i = 0; i < 100; i++)
    {
        char p[512];
        strcpy(p, base);
        strcat(p, "/file_");
        char num[8];
        num[0] = (char)('0' + (i / 100) % 10);
        num[1] = (char)('0' + (i / 10) % 10);
        num[2] = (char)('0' + i % 10);
        num[3] = '\0';
        strcat(p, num);
        strcat(p, "_with_long_name.txt");

        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 3) { many_ok = false; break; }
        write(fd, "y", 1);
        close(fd);
    }
    check("100 long-named files in one directory", many_ok);

    // getdents64 sees them all with the right case.
    DIR* d = opendir(base);
    check("opendir(lfn)", d != NULL);
    int seen_named = 0, seen_files = 0;
    bool case_ok = true;
    if (d)
    {
        struct dirent* de;
        while ((de = readdir(d)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            for (int i = 0; i < NN; i++)
                if (strcmp(de->d_name, names[i]) == 0)
                    seen_named++;
            if (strncmp(de->d_name, "file_", 5) == 0)
                seen_files++;

            // The exact-case originals must appear verbatim.
            if (strcmp(de->d_name, "LONGFILENAME.TXT") == 0)
                case_ok = false;
        }
        closedir(d);
    }
    check("getdents64 lists all 5 LFN names", seen_named == NN);
    {
        char dbg[32];
        dbg[0]=0;
        // print the count
        print("    seen_files=");
        print_i64(seen_files);
        print("\n");
    }
    check("getdents64 lists all 100 generated files", seen_files == 100);
    check("case is preserved (no 8.3 folding)", case_ok);

    // Case-insensitive lookup.
    char p[512];
    strcpy(p, base);
    strcat(p, "/LONG FILE NAME.TXT");
    struct stat st;
    check("case-insensitive lookup finds 'Long File Name.txt'",
          stat(p, &st) == 0);

    // A lowercase 8.3 name keeps its case through the NT flags.
    strcpy(p, base);
    strcat(p, "/lower.c");
    DIR* d2 = opendir(base);
    bool found_lower = false;
    if (d2)
    {
        struct dirent* de;
        while ((de = readdir(d2)) != NULL)
            if (strcmp(de->d_name, "lower.c") == 0)
                found_lower = true;
        closedir(d2);
    }
    check("lowercase 8.3 name 'lower.c' keeps its case", found_lower);
}

static void test_dirs()
{
    section("mkdir / rmdir / rename / unlink of an open file");

    char a[256], b[256];
    strcpy(a, D);
    strcat(a, "/dir_a");
    strcpy(b, D);
    strcat(b, "/dir_b");
    rmdir(a);
    rmdir(b);

    check("mkdir dir_a", mkdir(a, 0755) == 0);

    errno = 0;
    check("rmdir of a non-empty dir fails", mkdir(a, 0755) == -1 && errno == EEXIST);

    // Put a file inside, rmdir must fail ENOTEMPTY.
    char f[300];
    strcpy(f, a);
    strcat(f, "/inner.txt");
    int fd = open(f, O_WRONLY | O_CREAT, 0644);
    write(fd, "z", 1);
    close(fd);

    errno = 0;
    check("rmdir non-empty -> ENOTEMPTY", rmdir(a) == -1 && errno == ENOTEMPTY);

    // Rename across directories.
    char f2[300];
    strcpy(f2, D);
    strcat(f2, "/moved.txt");
    unlink(f2);
    check("rename across directories", rename(f, f2) == 0);
    struct stat st;
    check("moved file exists at the target", stat(f2, &st) == 0 && st.st_size == 1);
    errno = 0;
    check("source is gone", stat(f, &st) == -1 && errno == ENOENT);

    // Rename over an existing file.
    char f3[300];
    strcpy(f3, D);
    strcat(f3, "/target.txt");
    fd = open(f3, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "old", 3);
    close(fd);
    check("rename replaces an existing file", rename(f2, f3) == 0);
    fd = open(f3, O_RDONLY);
    char buf[8];
    ssize_t r = read(fd, buf, 8);
    close(fd);
    check("replacement has the new content", r == 1 && buf[0] == 'z');

    // Now dir_a is empty: rmdir works.
    check("rmdir of an empty dir", rmdir(a) == 0);

    // unlink of an open file: reads keep working, space frees at close.
    strcpy(f, D);
    strcat(f, "/unlinked.bin");
    unlink(f);
    fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "0123456789", 10);

    check("unlink of an open file succeeds", unlink(f) == 0);
    errno = 0;
    check("... and the name is gone", stat(f, &st) == -1 && errno == ENOENT);

    lseek(fd, 0, SEEK_SET);
    r = read(fd, buf, 10);
    check("the open fd still reads its data", r == 10 && buf[9] == '9');
    check("write through the unlinked fd still works",
          pwrite(fd, "Q", 1, 0) == 1);
    close(fd);
    errno = 0;
    check("after close the file is really gone",
          open(f, O_RDONLY) == -1 && errno == ENOENT);
}

static void test_cwd()
{
    section("getcwd / chdir / relative paths / .. over /dev / openat");

    char cwd[PATH_MAX_TEST];
    check("getcwd starts at the test dir or root", getcwd(cwd, sizeof(cwd)) != NULL);

    check("chdir to the test dir", chdir(D) == 0);
    check("getcwd reports it", getcwd(cwd, sizeof(cwd)) != NULL &&
          strcmp(cwd, D) == 0);

    // Relative path.
    int fd = open("basic.txt", O_RDONLY);
    check("relative open after chdir", fd >= 3);
    if (fd >= 3) close(fd);

    // .. through the /dev mount point: chdir /dev, then ".." lands in /.
    check("chdir /dev", chdir("/dev") == 0);
    check("chdir .. from /dev lands in /", chdir("..") == 0);
    check("getcwd is /", getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/") == 0);
    // ".." of / stays /.
    check("chdir .. from / stays at /", chdir("..") == 0 &&
          getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/") == 0);

    // openat with a dirfd.
    int dirfd = open("/test", O_RDONLY | O_DIRECTORY);
    check("open(/test, O_DIRECTORY)", dirfd >= 3);
    fd = openat(dirfd, "hello.txt", O_RDONLY);
    check("openat(dirfd, 'hello.txt')", fd >= 3);
    if (fd >= 3) close(fd);
    close(dirfd);

    // Trailing slash requires a directory; duplicate slashes are fine.
    errno = 0;
    fd = open("/test//hello.txt/", O_RDONLY);
    check("trailing slash on a file -> ENOTDIR", fd == -1 && errno == ENOTDIR);
    fd = open("/test//hello.txt", O_RDONLY);
    check("duplicate slashes work", fd >= 3);
    if (fd >= 3) close(fd);

    chdir("/");
}

static void test_stat()
{
    section("stat / fstat consistency");

    char path[256];
    strcpy(path, D);
    strcat(path, "/basic.txt");

    struct stat a, b;
    check("stat", stat(path, &a) == 0);
    int fd = open(path, O_RDONLY);
    check("fstat", fstat(fd, &b) == 0);
    close(fd);

    check("st_size agrees", a.st_size == b.st_size && a.st_size > 0);
    check("S_ISREG", S_ISREG(a.st_mode));
    check("st_mtime is plausible (>= 2020 epoch)",
          a.st_mtim.tv_sec > 1577836800LL);

    check("stat of / is a directory", stat("/", &a) == 0 && S_ISDIR(a.st_mode));

    // st_dev must tell the volumes apart: (st_dev, st_ino) is what "is this
    // the same file?" is built on, and devfs used to share a constant with
    // the FAT root.
    struct stat rootst, devst;
    check("stat / and /dev both succeed",
          stat("/", &rootst) == 0 && stat("/dev", &devst) == 0);
    check("st_dev differs between the root volume and devfs",
          rootst.st_dev != devst.st_dev);
    check("st_dev is non-zero on both", rootst.st_dev != 0 && devst.st_dev != 0);
    check("stat of /dev/tty is a chrdev",
          stat("/dev/tty", &a) == 0 && S_ISCHR(a.st_mode));
    check("st_ino is non-zero", a.st_ino != 0);

    // Two opens of one file see one size through the shared vnode.
    strcpy(path, D);
    strcat(path, "/shared.bin");
    int w = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(w, "01234567", 8);
    int r = open(path, O_RDONLY);
    fstat(r, &a);
    check("second open sees size 8 (shared vnode)", a.st_size == 8);
    write(w, "89", 2);
    fstat(r, &a);
    check("... and sees the write immediately (size 10)", a.st_size == 10);
    close(r);
    close(w);
}

static void test_errors()
{
    section("error paths");

    struct stat st;
    errno = 0;
    check("ENOENT for a missing file",
          stat("/no/such/file", &st) == -1 && errno == ENOENT);

    errno = 0;
    check("ENOTDIR when a component is a file",
          stat("/test/hello.txt/x", &st) == -1 && errno == ENOTDIR);

    errno = 0;
    int fd = open("/test", O_RDONLY);
    char buf[8];
    check("read of a directory fd -> EISDIR",
          fd >= 3 && read(fd, buf, 8) == -1 && errno == EISDIR);
    if (fd >= 3) close(fd);

    fd = open("/test", O_RDONLY);
    errno = 0;
    check("write to an O_RDONLY fd -> EBADF",
          fd >= 3 && write(fd, buf, 1) == -1 && errno == EBADF);
    if (fd >= 3) close(fd);

    errno = 0;
    check("close of a bogus fd -> EBADF", close(1234) == -1 && errno == EBADF);

    errno = 0;
    check("EFAULT: stat into kernel memory",
          syscall(SYS_STAT, (uint64_t)"/test/hello.txt", 0x200000) == -EFAULT);

    // ENAMETOOLONG: a 300-char component.
    char longp[400];
    longp[0] = '/';
    for (int i = 1; i < 301; i++)
        longp[i] = 'x';
    longp[301] = '\0';
    errno = 0;
    fd = open(longp, O_RDONLY);
    check("ENAMETOOLONG for a 300-char name",
          fd == -1 && errno == ENAMETOOLONG);

    // ESPIPE on the tty.
    errno = 0;
    off_t o = lseek(0, 0, SEEK_CUR);
    check("lseek on the tty -> ESPIPE", o == -1 && errno == ESPIPE);

    // open("dir/", O_CREAT) -> EISDIR.
    errno = 0;
    fd = open("/test/newdir/", O_CREAT | O_WRONLY, 0755);
    check("open('dir/', O_CREAT) -> EISDIR", fd == -1 && errno == EISDIR);

    // unlink of a directory -> EISDIR.
    errno = 0;
    check("unlink of a directory -> EISDIR",
          unlink("/test") == -1 && errno == EISDIR);
}

static void test_mfile()
{
    section("EMFILE: the 65th open");

    char path[256];
    strcpy(path, D);
    strcat(path, "/mfile.bin");
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "m", 1);

    int fds[80];
    int n = 0;
    // fd 0,1,2 and `fd` itself occupy 4 slots: 60 more fit in 64.
    while (n < 80)
    {
        int f = dup(fd);
        if (f < 0)
            break;
        fds[n++] = f;
    }
    check("exactly 64 - 4 = 60 dups fit", n == 60);
    errno = 0;
    int over = dup(fd);
    check("the 65th descriptor fails", over == -1);
    check("... with EMFILE", errno == EMFILE);

    for (int i = 0; i < n; i++)
        close(fds[i]);
    close(fd);
}

static void test_dev()
{
    section("/dev/null, /dev/zero, isatty");

    char buf[16];

    int fd = open("/dev/null", O_RDWR);
    check("open /dev/null", fd >= 3);
    check("read /dev/null -> 0 (EOF)", read(fd, buf, 16) == 0);
    check("write /dev/null is swallowed", write(fd, "abc", 3) == 3);
    check("/dev/null is not a tty", isatty(fd) == 0);
    close(fd);

    fd = open("/dev/zero", O_RDONLY);
    check("open /dev/zero", fd >= 3);
    ssize_t r = read(fd, buf, 16);
    bool zero = r == 16;
    for (int i = 0; zero && i < 16; i++)
        if (buf[i] != 0) zero = false;
    check("read /dev/zero -> 16 zero bytes", zero);
    close(fd);

    check("isatty(0) == 1", isatty(0) == 1);
    check("isatty(1) == 1", isatty(1) == 1);

    fd = open("/test/hello.txt", O_RDONLY);
    check("isatty of a file fd == 0", fd >= 3 && isatty(fd) == 0);
    if (fd >= 3) close(fd);

    // TCGETS returns a canonical termios.
    struct termios t;
    check("ioctl(TCGETS) on the tty",
          syscall(SYS_IOCTL, 0, (uint64_t)TCGETS, (uint64_t)&t) == 0);
    check("termios says ICANON|ECHO",
          (t.c_lflag & ICANON) != 0 && (t.c_lflag & ECHO) != 0);
    int nfd = open("/dev/null", O_RDWR);
    errno = 0;
    sint64_t rc = syscall(SYS_IOCTL, (uint64_t)nfd, (uint64_t)TCGETS,
                          (uint64_t)&t);
    check("TCGETS on /dev/null -> ENOTTY", rc == -ENOTTY);
    close(nfd);
}

static void test_rw_vectors()
{
    section("readv / writev");

    char path[256];
    strcpy(path, D);
    strcat(path, "/iov.bin");

    struct iovec_local
    {
        uint64_t base;
        uint64_t len;
    };

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("create iov.bin", fd >= 3);

    char a[] = "AAAA";
    char b[] = "BB";
    char c[] = "CCCC";
    iovec_local wv[3] =
    {
        { (uint64_t)(uintptr_t)a, 4 },
        { (uint64_t)(uintptr_t)b, 2 },
        { (uint64_t)(uintptr_t)c, 4 },
    };
    sint64_t r = syscall(SYS_WRITEV, (uint64_t)fd, (uint64_t)wv, 3);
    check("writev writes 10 bytes", r == 10);

    char out[16];
    iovec_local rv[2] =
    {
        { (uint64_t)(uintptr_t)out, 6 },
        { (uint64_t)(uintptr_t)(out + 6), 4 },
    };
    lseek(fd, 0, SEEK_SET);
    r = syscall(SYS_READV, (uint64_t)fd, (uint64_t)rv, 2);
    check("readv reads 10 bytes", r == 10);
    out[10] = '\0';
    check("content is AAAABBCCCC", strcmp(out, "AAAABBCCCC") == 0);
    close(fd);
}

static void test_umask_chmod()
{
    section("umask / chmod");

    mode_t old = umask(0077);
    check("umask returns the old value", old == 022);

    char path[256];
    strcpy(path, D);
    strcat(path, "/masked.txt");
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT, 0666);
    close(fd);

    struct stat st;
    stat(path, &st);
    check("umask 0077 strips group/other bits", (st.st_mode & 0777) == 0600);

    umask(022);

    check("chmod 0444", chmod(path, 0444) == 0);
    stat(path, &st);
    check("... reflected in st_mode", (st.st_mode & 0777) == 0444);

    // Read-only files reject writes.
    errno = 0;
    fd = open(path, O_WRONLY);
    check("open(O_WRONLY) of a chmod-0444 file -> EACCES",
          fd == -1 && errno == EACCES);

    check("chmod back to 0644", chmod(path, 0644) == 0);
    fd = open(path, O_WRONLY);
    check("... and now it opens", fd >= 3);
    if (fd >= 3) close(fd);
}

// ===========================================================================
// verify mode: everything the suite wrote must still be there after a reboot.

static int do_verify()
{
    section("verify (persistence after restart)");

    struct stat st;

    char path[256];
    strcpy(path, D);
    strcat(path, "/basic.txt");
    check("basic.txt persisted with its content size",
          stat(path, &st) == 0 && st.st_size == 17);

    strcpy(path, D);
    strcat(path, "/big.bin");
    check("big.bin persisted at 3 MiB", stat(path, &st) == 0 &&
          st.st_size == 3 * 1024 * 1024);

    int fd = open(path, O_RDONLY);
    uint8_t* buf = (uint8_t*)malloc(1000);
    bool content_ok = fd >= 3 && buf != NULL;
    if (content_ok)
    {
        uint32_t seed = 12345;
        for (int i = 0; i < 8 && content_ok; i++)
        {
            seed = seed * 1103515245 + 12345;
            uint32_t pos = (seed >> 7) % (3 * 1024 * 1024 - 1000);
            lseek(fd, pos, SEEK_SET);
            ssize_t r = read(fd, buf, 1000);
            content_ok = r == 1000 && same(buf, pos, 1000);
        }
    }
    check("big.bin content survived the restart", content_ok);
    if (fd >= 3) close(fd);
    if (buf) free(buf);

    strcpy(path, D);
    strcat(path, "/lfn");
    DIR* d = opendir(path);
    int seen_files = 0;
    bool saw_lfn = false;
    if (d)
    {
        struct dirent* de;
        while ((de = readdir(d)) != NULL)
        {
            if (strncmp(de->d_name, "file_", 5) == 0)
                seen_files++;

            if (strcmp(de->d_name, "Long File Name.txt") == 0)
                saw_lfn = true;
        }
        closedir(d);
    }
    check("all 100 generated LFN files persisted", seen_files == 100);
    check("the mixed-case LFN persisted verbatim", saw_lfn);

    print("\nfstest verify: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");
    return failed;
}

// ===========================================================================

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "verify") == 0)
        return do_verify();

    if (argc >= 2 && strcmp(argv[1], "cloexec") == 0)
        return cloexec_mode();

    print("fstest - fd layer, VFS and FAT32 checks\n");

    // Fresh test directory on every run.
    mkdir("/test", 0755);
    mkdir(D, 0755);

    test_basic_rw();
    test_big_file();
    test_flags();
    test_offsets();
    test_cloexec(argv[0]);
    test_lfn();
    test_dirs();
    test_cwd();
    test_stat();
    test_errors();
    test_mfile();
    test_dev();
    test_rw_vectors();
    test_umask_chmod();
    test_dup_fcntl_regressions();
    test_rename_coherency();

    // Make everything durable: the harness reboots and runs `fstest verify`.
    sync();

    print("\nfstest: ");
    print_i64(passed);
    print(" passed, ");
    print_i64(failed);
    print(" failed\n");
    return failed;
}
