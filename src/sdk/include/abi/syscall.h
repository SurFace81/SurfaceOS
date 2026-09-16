#ifndef ABI_SYSCALL_H
#define ABI_SYSCALL_H

// Syscall numbers (int 0x80). Arguments: rdi, rsi, rdx. Result: rax,
// (uint64_t)-1 on error.

#define SYS_EXIT            0   // (code)

#define SYS_WRITE           1   // (str)
#define SYS_SET_CURSOR      2   // (col, row)
#define SYS_CLEAR           3

#define SYS_READ_KEY        4   // (keyboard_event_t*)            blocks
#define SYS_READ_LINE       5   // (buf, max_len) -> length       blocks

#define SYS_WRITE_FILE      6   // (path, data, size)
#define SYS_READ_FILE       7   // (path, buf, max_size)
#define SYS_STAT_FILE       8   // (path, file_stat_t*)
#define SYS_READ_DIR        9   // (path, dir_entry_t*, max)

#define SYS_UPTIME          10  // (uptime_t*)
#define SYS_TIME            11  // (datetime_t*)

#define SYS_BRK             12  // (new_brk or 0) -> current break

// Processes
#define SYS_GETPID          13  // () -> pid
#define SYS_GETPPID         14  // () -> parent pid (0: started by the console)
#define SYS_FORK            15  // () -> child pid in parent, 0 in child
#define SYS_EXEC            16  // (path, argv) -> only returns on error
#define SYS_WAITPID         17  // (pid or -1, int* status, options) -> pid, 0 with WNOHANG
#define SYS_YIELD           18  // ()
#define SYS_SLEEP           19  // (milliseconds)
#define SYS_KILL            20  // (pid) -> terminate with status EXIT_KILLED

// Memory
#define SYS_MMAP            21  // (addr hint, length, prot) -> address
#define SYS_MUNMAP          22  // (addr, length)
#define SYS_MPROTECT        23  // (addr, length, prot)

#endif
