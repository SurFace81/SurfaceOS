#include "../include/unistd.h"
#include "../include/mman.h"
#include "../include/syscall.h"
#include "../include/abi/syscall.h"
#include "../include/abi/time.h"
#include "../include/abi/termios.h"   // TIOCGPGRP/TIOCSPGRP

pid_t getpid()  { return (pid_t)syscall(SYS_GETPID); }
pid_t getppid() { return (pid_t)syscall(SYS_GETPPID); }
pid_t fork()    { return (pid_t)__syscall_ret(syscall(SYS_FORK)); }

int execv(const char* path, char* const argv[])
{
    extern char** environ;
    return execve(path, argv, environ);
}

int execve(const char* path, char* const argv[], char* const envp[])
{
    return (int)__syscall_ret(syscall(SYS_EXECVE, (uint64_t)path, (uint64_t)argv,
                                      (uint64_t)envp));
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return (pid_t)__syscall_ret(syscall(SYS_WAIT4, (uint64_t)(sint64_t)pid,
                                        (uint64_t)status, (uint64_t)options));
}

int kill(pid_t pid, int sig)
{
    return (int)__syscall_ret(syscall(SYS_KILL, (uint64_t)(sint64_t)pid,
                                      (uint64_t)(sint64_t)sig));
}

int setpgid(pid_t pid, pid_t pgid)
{
    return (int)__syscall_ret(syscall(SYS_SETPGID, (uint64_t)(sint64_t)pid,
                                      (uint64_t)(sint64_t)pgid));
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)__syscall_ret(syscall(SYS_GETPGID, (uint64_t)(sint64_t)pid));
}

pid_t getpgrp()  { return (pid_t)syscall(SYS_GETPGRP); }
pid_t setsid()   { return (pid_t)__syscall_ret(syscall(SYS_SETSID)); }

uid_t getuid()   { return (uid_t)syscall(SYS_GETUID); }
uid_t geteuid()  { return (uid_t)syscall(SYS_GETEUID); }
gid_t getgid()   { return (gid_t)syscall(SYS_GETGID); }
gid_t getegid()  { return (gid_t)syscall(SYS_GETEGID); }

pid_t tcgetpgrp(int fd)
{
    pid_t pgrp = 0;
    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0)
        return -1;
    return pgrp;
}

int tcsetpgrp(int fd, pid_t pgid)
{
    return ioctl(fd, TIOCSPGRP, &pgid);
}

void yield()                { syscall(SYS_SCHED_YIELD); }

void sleep_ms(uint32_t ms)
{
    timespec ts;
    ts.tv_sec  = (sint64_t)(ms / 1000);
    ts.tv_nsec = (sint64_t)(ms % 1000) * 1000000;
    syscall(SYS_NANOSLEEP, (uint64_t)&ts);
}

void* mmap(void* addr, size_t length, int prot)
{
    sint64_t r = __syscall_ret(syscall(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot));
    return r < 0 ? MAP_FAILED : (void*)(uint64_t)r;
}

int munmap(void* addr, size_t length)
{
    return (int)__syscall_ret(syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length));
}

int mprotect(void* addr, size_t length, int prot)
{
    return (int)__syscall_ret(syscall(SYS_MPROTECT, (uint64_t)addr, (uint64_t)length, (uint64_t)prot));
}
