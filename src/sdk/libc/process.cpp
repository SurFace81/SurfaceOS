#include "../include/unistd.h"
#include "../include/mman.h"
#include "../include/syscall.h"
#include "../include/abi/syscall.h"
#include "../include/abi/time.h"

pid_t getpid()  { return (pid_t)syscall(SYS_GETPID); }
pid_t getppid() { return (pid_t)syscall(SYS_GETPPID); }
pid_t fork()    { return (pid_t)__syscall_ret(syscall(SYS_FORK)); }

int execv(const char* path, char* const argv[])
{
    return (int)__syscall_ret(syscall(SYS_EXECVE, (uint64_t)path, (uint64_t)argv));
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return (pid_t)__syscall_ret(syscall(SYS_WAIT4, (uint64_t)(sint64_t)pid,
                                        (uint64_t)status, (uint64_t)options));
}

int  kill(pid_t pid)        { return (int)__syscall_ret(syscall(SYS_KILL, (uint64_t)(sint64_t)pid)); }
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
