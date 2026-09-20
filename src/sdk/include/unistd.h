#ifndef SFOS_UNISTD_H
#define SFOS_UNISTD_H

#include "abi/types.h"
#include "abi/process.h"

pid_t    getpid();
pid_t    getppid();

// Duplicate the calling process. Returns the child's pid in the parent, 0 in
// the child, -1 on failure. Memory is copied, not shared.
pid_t    fork();

// Replace the current program. argv is NULL-terminated; argv may be NULL.
// Returns only on failure (-1, errno set), in which case the old program
// keeps running.
int      execv(const char* path, char* const argv[]);
int      execve(const char* path, char* const argv[], char* const envp[]);

// Wait for a child (pid > 0) or any child (pid == -1). *status receives the
// Linux wait(2) status: decode with WIFEXITED/WEXITSTATUS and
// WIFSIGNALED/WTERMSIG (abi/process.h). With WNOHANG returns 0 if no child
// has finished yet. -1 if there is nothing to wait for.
pid_t    waitpid(pid_t pid, int* status, int options);

// Terminate a process of the same session (as if by SIGKILL).
int      kill(pid_t pid);

void     yield();
void     sleep_ms(uint32_t ms);

#endif
