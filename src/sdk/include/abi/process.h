#ifndef ABI_PROCESS_H
#define ABI_PROCESS_H

#include "types.h"

// Shared between the kernel and the SDK.

typedef sint32_t pid_t;

// wait4 options
#define WNOHANG             1

// ---------------------------------------------------------------------------
// Exit status encoding: exactly Linux wait(2).
//
//   normal exit(code)    -> (code & 0xff) << 8           (WIFEXITED)
//   terminated by signal -> signal number in the low 7 bits (WIFSIGNALED)
//
// Signals do not exist yet (stage 4+); the kernel encodes the killing event
// as a signal number so WEXITSTATUS/WTERMSIG in musl work from day one:
//   Esc session kill -> SIGINT, kill() -> SIGKILL,
//   #PF/#GP -> SIGSEGV, #UD -> SIGILL, #DE -> SIGFPE.
// ---------------------------------------------------------------------------
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WTERMSIG(status)    ((status) & 0x7F)
#define WIFSIGNALED(status) (((sint8_t)((((status) & 0x7F) + 1) >> 1)) > 0)
#define WIFSTOPPED(status)  (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)

// Signal numbers (Linux).
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGSEGV     11
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17

// mmap / mprotect protection bits
#define PROT_NONE           0
#define PROT_READ           1
#define PROT_WRITE          2
#define PROT_EXEC           4

// mmap flags (Linux x86_64 values)
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20

#define MAP_FAILED          ((void*)-1)

// Limits
#define ARG_MAX             (128 * 1024)    // argv+envp strings and pointers,
                                            // copied onto the new stack

#endif
