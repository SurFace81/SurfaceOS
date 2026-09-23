#ifndef ABI_PROCESS_H
#define ABI_PROCESS_H

#include "types.h"
#include "signal.h"     // signal numbers: a wait status names one

// Shared between the kernel and the SDK.

typedef sint32_t pid_t;

// wait4 options
#define WNOHANG             1
#define WUNTRACED           2   // also report children stopped by a signal
#define WCONTINUED          8   // also report children resumed by SIGCONT

// ---------------------------------------------------------------------------
// Exit status encoding: exactly Linux wait(2).
//
//   normal exit(code)    -> (code & 0xff) << 8           (WIFEXITED)
//   terminated by signal -> signal number in the low 7 bits (WIFSIGNALED)
//   stopped by a signal  -> 0x7f | (signal << 8)         (WIFSTOPPED)
//   resumed by SIGCONT   -> 0xffff                       (WIFCONTINUED)
// ---------------------------------------------------------------------------
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WTERMSIG(status)    ((status) & 0x7F)
#define WIFSIGNALED(status) (((sint8_t)((((status) & 0x7F) + 1) >> 1)) > 0)
#define WIFSTOPPED(status)  (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)
#define WIFCONTINUED(status) ((status) == 0xFFFF)

// Signal numbers live in abi/signal.h, included above.

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
