#ifndef ABI_PROCESS_H
#define ABI_PROCESS_H

#include "types.h"

// Shared between the kernel and the SDK.

typedef sint32_t pid_t;

// waitpid options
#define WNOHANG             1

// Exit status reported by waitpid() / the console. A normal exit reports the
// value passed to exit() or returned from main(). A process that did not exit
// on its own reports one of these instead.
// (Transitional: replaced by the Linux (code<<8)/signal encoding in step 3.0.2)
#define EXIT_ESCAPE         130                 // terminated with Esc
#define EXIT_KILLED         137                 // terminated with kill()
#define EXIT_FAULT_BASE     200                 // + CPU exception vector
#define EXIT_FAULT_PAGE     (EXIT_FAULT_BASE + 14)
#define EXIT_FAULT_GP       (EXIT_FAULT_BASE + 13)
#define EXIT_FAULT_UD       (EXIT_FAULT_BASE + 6)
#define EXIT_FAULT_DE       (EXIT_FAULT_BASE + 0)

// mmap / mprotect protection bits
#define PROT_NONE           0
#define PROT_READ           1
#define PROT_WRITE          2
#define PROT_EXEC           4

#define MAP_FAILED          ((void*)-1)

// Limits
#define ARG_MAX_COUNT       16      // argv entries, including argv[0]
#define ARG_MAX_BYTES       1024    // all argv strings together

// What the kernel passes to _start in rdi.
struct program_info
{
    uint64_t heap_start;    // initial program break
    uint64_t heap_size;     // how far brk may grow
    uint64_t argc;
    char**   argv;          // argv[argc] == NULL
};

#endif
