#include "../include/unistd.h"
#include "../include/mman.h"
#include "../include/abi/syscall.h"

extern uint64_t syscall(uint64_t num, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);

pid_t getpid()  { return (pid_t)syscall(SYS_GETPID); }
pid_t getppid() { return (pid_t)syscall(SYS_GETPPID); }
pid_t fork()    { return (pid_t)syscall(SYS_FORK); }

int execv(const char* path, char* const argv[])
{
    return (int)syscall(SYS_EXEC, (uint64_t)path, (uint64_t)argv);
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return (pid_t)syscall(SYS_WAITPID, (uint64_t)(sint64_t)pid, (uint64_t)status, (uint64_t)options);
}

int  kill(pid_t pid)        { return (int)syscall(SYS_KILL, (uint64_t)(sint64_t)pid); }
void yield()                { syscall(SYS_YIELD); }
void sleep_ms(uint32_t ms)  { syscall(SYS_SLEEP, (uint64_t)ms); }

void* mmap(void* addr, size_t length, int prot)
{
    return (void*)syscall(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot);
}

int munmap(void* addr, size_t length)
{
    return (int)syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length);
}

int mprotect(void* addr, size_t length, int prot)
{
    return (int)syscall(SYS_MPROTECT, (uint64_t)addr, (uint64_t)length, (uint64_t)prot);
}
