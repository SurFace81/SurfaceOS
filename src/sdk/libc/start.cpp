#include "../include/syscall.h"
#include "../include/abi/types.h"
#include "../include/abi/syscall.h"

// The process environment (envp), published for getenv(); the definition
// lives in stdlib.cpp together with the rest of the environment support.
extern char** environ;

extern int main(int argc, char** argv);

extern "C" void __libc_start(int argc, char** argv, char** envp)
{
    environ = envp;
    int code = main(argc, argv);
    syscall(SYS_EXIT_GROUP, (uint64_t)(sint32_t)code);
    while (1) {}
}
