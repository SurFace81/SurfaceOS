// POSIX file-descriptor wrappers (stage 3.7 SDK).
//
// Each sets `errno` and returns the documented failure value (-1 / NULL)
// like musl will, so apps written for Linux compile unchanged. The raw
// syscall already returns >= 0 or -errno; __syscall_ret converts.

#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/syscall.h"
#include "../include/abi/syscall.h"
#include "../include/abi/stat.h"
#include "../include/abi/termios.h"

// --- open / creat (variadic mode) ------------------------------------------

int open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = (__builtin_va_arg(ap, unsigned));
        __builtin_va_end(ap);
    }
    return (int)__syscall_ret(syscall(SYS_OPEN, (uint64_t)path,
                                      (uint64_t)flags, (uint64_t)mode));
}

int openat(int dirfd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = (__builtin_va_arg(ap, unsigned));
        __builtin_va_end(ap);
    }
    return (int)__syscall_ret(syscall(SYS_OPENAT, (uint64_t)(sint64_t)dirfd,
                                      (uint64_t)path, (uint64_t)flags,
                                      (uint64_t)mode));
}

int creat(const char* path, mode_t mode)
{
    return (int)__syscall_ret(syscall(SYS_CREAT, (uint64_t)path, (uint64_t)mode));
}

// --- I/O --------------------------------------------------------------------

ssize_t read(int fd, void* buf, size_t count)
{
    return (ssize_t)__syscall_ret(syscall(SYS_READ, (uint64_t)fd,
                                          (uint64_t)buf, count));
}

ssize_t write(int fd, const void* buf, size_t count)
{
    return (ssize_t)__syscall_ret(syscall(SYS_WRITE, (uint64_t)fd,
                                          (uint64_t)buf, count));
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
    return (ssize_t)__syscall_ret(syscall(SYS_PREAD64, (uint64_t)fd,
                                          (uint64_t)buf, count,
                                          (uint64_t)offset));
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset)
{
    return (ssize_t)__syscall_ret(syscall(SYS_PWRITE64, (uint64_t)fd,
                                          (uint64_t)buf, count,
                                          (uint64_t)offset));
}

int close(int fd)
{
    return (int)__syscall_ret(syscall(SYS_CLOSE, (uint64_t)fd));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)__syscall_ret(syscall(SYS_LSEEK, (uint64_t)fd,
                                        (uint64_t)offset, (uint64_t)whence));
}

int dup(int oldfd)
{
    return (int)__syscall_ret(syscall(SYS_DUP, (uint64_t)oldfd));
}

int dup2(int oldfd, int newfd)
{
    return (int)__syscall_ret(syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd));
}

int dup3(int oldfd, int newfd, int flags)
{
    return (int)__syscall_ret(syscall(SYS_DUP3, (uint64_t)oldfd,
                                      (uint64_t)newfd, (uint64_t)flags));
}

// --- fcntl (variadic arg) ---------------------------------------------------

int fcntl(int fd, int cmd, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    uint64_t arg = __builtin_va_arg(ap, uint64_t);
    __builtin_va_end(ap);
    return (int)__syscall_ret(syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, arg));
}

// --- metadata ---------------------------------------------------------------

int stat(const char* path, struct stat* st)
{
    return (int)__syscall_ret(syscall(SYS_STAT, (uint64_t)path, (uint64_t)st));
}

int lstat(const char* path, struct stat* st)
{
    return (int)__syscall_ret(syscall(SYS_LSTAT, (uint64_t)path, (uint64_t)st));
}

int fstat(int fd, struct stat* st)
{
    return (int)__syscall_ret(syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)st));
}

int access(const char* path, int mode)
{
    return (int)__syscall_ret(syscall(SYS_ACCESS, (uint64_t)path, (uint64_t)mode));
}

int chmod(const char* path, mode_t mode)
{
    return (int)__syscall_ret(syscall(SYS_CHMOD, (uint64_t)path, (uint64_t)mode));
}

int fchmod(int fd, mode_t mode)
{
    return (int)__syscall_ret(syscall(SYS_FCHMOD, (uint64_t)fd, (uint64_t)mode));
}

mode_t umask(mode_t mask)
{
    // Never fails.
    return (mode_t)syscall(SYS_UMASK, (uint64_t)mask);
}

int mkdir(const char* path, mode_t mode)
{
    return (int)__syscall_ret(syscall(SYS_MKDIR, (uint64_t)path, (uint64_t)mode));
}

int unlink(const char* path)
{
    return (int)__syscall_ret(syscall(SYS_UNLINK, (uint64_t)path));
}

int rmdir(const char* path)
{
    return (int)__syscall_ret(syscall(SYS_RMDIR, (uint64_t)path));
}

int rename(const char* oldp, const char* newp)
{
    return (int)__syscall_ret(syscall(SYS_RENAME, (uint64_t)oldp, (uint64_t)newp));
}

int fsync(int fd)
{
    return (int)__syscall_ret(syscall(SYS_FSYNC, (uint64_t)fd));
}

int ftruncate(int fd, off_t length)
{
    return (int)__syscall_ret(syscall(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length));
}

int truncate(const char* path, off_t length)
{
    return (int)__syscall_ret(syscall(SYS_TRUNCATE, (uint64_t)path, (uint64_t)length));
}

void sync()
{
    syscall(SYS_SYNC);
}

int chdir(const char* path)
{
    return (int)__syscall_ret(syscall(SYS_CHDIR, (uint64_t)path));
}

int fchdir(int fd)
{
    return (int)__syscall_ret(syscall(SYS_FCHDIR, (uint64_t)fd));
}

char* getcwd(char* buf, size_t size)
{
    sint64_t r = __syscall_ret(syscall(SYS_GETCWD, (uint64_t)buf, size));
    return r < 0 ? (char*)NULL : buf;
}

int isatty(int fd)
{
    struct termios t;
    sint64_t r = __syscall_ret(syscall(SYS_IOCTL, (uint64_t)fd,
                                       (uint64_t)TCGETS, (uint64_t)&t));
    return r < 0 ? 0 : 1;
}

int ioctl(int fd, unsigned long request, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    uint64_t arg = __builtin_va_arg(ap, uint64_t);
    __builtin_va_end(ap);
    return (int)__syscall_ret(syscall(SYS_IOCTL, (uint64_t)fd, request, arg));
}
