#ifndef ABI_SYSCALL_H
#define ABI_SYSCALL_H

// Syscall ABI (int 0x80 until stage 6 adds the `syscall` instruction).
//
//   arguments: rdi, rsi, rdx, r10, r8, r9  (Linux order, rcx is clobbered)
//   result:    rax, sint64_t. >= 0 on success, -errno on failure
//              (Linux x86_64 numbers and semantics; see abi/errno.h).
//
// Numbers 0..335 are the Linux x86_64 ones. SurfaceOS extensions that have
// no Linux equivalent live at 0x1000+ and disappear as the real interfaces
// (termios, clock_gettime, ...) arrive.

// --- Linux x86_64 numbers ---------------------------------------------------
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSTAT           6
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_IOCTL           16
#define SYS_PREAD64         17
#define SYS_PWRITE64        18
#define SYS_READV           19
#define SYS_WRITEV          20
#define SYS_ACCESS          21
#define SYS_SCHED_YIELD     24
#define SYS_DUP             32
#define SYS_DUP2            33
#define SYS_NANOSLEEP       35
#define SYS_GETPID          39
#define SYS_FORK            57
#define SYS_EXECVE          59
#define SYS_EXIT            60
#define SYS_WAIT4           61
#define SYS_KILL            62
#define SYS_FCNTL           72
#define SYS_FSYNC           74
#define SYS_FDATASYNC       75
#define SYS_TRUNCATE        76
#define SYS_FTRUNCATE       77
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_FCHDIR          81
#define SYS_RENAME          82
#define SYS_MKDIR           83
#define SYS_RMDIR           84
#define SYS_CREAT           85
#define SYS_UNLINK          87
#define SYS_READLINK        89
#define SYS_CHMOD           90
#define SYS_FCHMOD          91
#define SYS_CHOWN           92
#define SYS_FCHOWN          93
#define SYS_UMASK           95
#define SYS_GETPPID         110
#define SYS_SYNC            162
#define SYS_GETDENTS64      217
#define SYS_EXIT_GROUP      231
#define SYS_OPENAT          257
#define SYS_MKDIRAT         258
#define SYS_NEWFSTATAT      262
#define SYS_UNLINKAT        263
#define SYS_RENAMEAT        264
#define SYS_READLINKAT      267
#define SYS_FACCESSAT       269
#define SYS_UTIMENSAT       280
#define SYS_DUP3            292
#define SYS_RENAMEAT2       316

// Reserved for stage 6 (musl); the dispatcher returns -ENOSYS until then.
#define SYS_ARCH_PRCTL      158
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME   228
#define SYS_GETRANDOM       318

// --- SurfaceOS extensions (0x1000+) -----------------------------------------
// Legacy console/keyboard/file helpers. Each of these is replaced by a real
// POSIX interface in a later stage and then deleted.
#define SYSX_READ_KEY       0x1000  // (keyboard_event_t*)            blocks
#define SYSX_SET_CURSOR     0x1001  // (col, row)
#define SYSX_CLEAR          0x1002  // ()
#define SYSX_UPTIME         0x1003  // (uptime_t*)
#define SYSX_TIME           0x1004  // (datetime_t*)
#define SYSX_READ_LINE      0x1005  // (buf, max_len) -> length       blocks
#define SYSX_WRITE_FILE     0x1006  // (path, data, size)   whole-file, legacy
#define SYSX_READ_FILE      0x1007  // (path, buf, max_size)
#define SYSX_STAT_FILE      0x1008  // (path, file_stat_t*)
#define SYSX_READ_DIR       0x1009  // (path, dir_entry_t*, max_entries)

#endif // ABI_SYSCALL_H
