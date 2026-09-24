#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "context.h"
#include "paging.h"
#include "../drivers/keyboard.h"
#include "../../sdk/include/abi/process.h"

// Opaque handles defined in fs/file.h and fs/vfs.h.
struct fd_table;
struct vnode;

// ---------------------------------------------------------------------------
// User address space layout (lower half, [USER_MIN, USER_LIMIT), see paging.h)
// ---------------------------------------------------------------------------
//
//   0x0000000000000000  NULL guard, never mapped            (below USER_MIN)
//   0x0000000000400000  ELF image (PT_LOAD segments, see src/sdk/linker.ld)
//   image_end           heap, grows up via SYS_BRK          (up to USER_MMAP_BASE)
//   0x0000000010000000  anonymous mmap window               (USER_MMAP_SIZE)
//   ...                 unused
//   stack bottom        stack, grows down, NX               (USER_STACK_SIZE)
//   0x00007FFFFFFFF000  USER_STACK_TOP; one unmapped guard page above it
//
// Every address space has the whole lower half to itself, so all programs
// link at the same address. The regions are sized independently: the mmap
// window is a fixed span because sys_mmap scans it page by page, and the
// stack sits at the top of the half so either can grow without moving the
// other. paging::map_user_page enforces [USER_MIN, USER_LIMIT).
//
// The argv/envp/auxv block execve() builds lives at the top of this same
// stack (the SysV ABI initial-process-stack layout), so there is no separate
// args region any more.
#define USER_IMAGE_VADDR    0x400000ULL             // must match src/sdk/linker.ld
#define USER_IMAGE_MAX      (64 * 1024 * 1024)      // largest executable file
#define USER_MMAP_BASE      0x10000000ULL
#define USER_MMAP_SIZE      0x30000000ULL           // 768 MiB
#define USER_MMAP_LIMIT     (USER_MMAP_BASE + USER_MMAP_SIZE)
#define USER_STACK_SIZE     (1024 * 1024)           // 1 MiB: args + program stack
#define USER_STACK_TOP      (USER_LIMIT - PAGE_SIZE_4K)

#define KERNEL_STACK_SIZE   (64 * 1024)
#define MAX_PROCESSES       32

// ---------------------------------------------------------------------------
// Scheduling model
// ---------------------------------------------------------------------------
//
// User code is preempted by the timer; kernel code is not. A process switch
// only ever happens at the boundary back to ring 3 - on return from a syscall,
// an IRQ that interrupted user code, or a user-mode fault - by rewriting the
// trap frame that is about to be popped. Consequences:
//
//   * the drivers (xHCI, FAT32, screen) never see reentrancy;
//   * a syscall that has to wait (read_key, waitpid) does not sleep inside
//     the kernel. It rewinds RIP over `int 0x80`, marks the process blocked
//     and switches away; when the process is woken it simply re-executes the
//     syscall with its registers intact.
//
// Each process nevertheless owns its kernel stack, and TSS RSP0 follows the
// switch. The restart trick only works while a syscall has committed no
// side effect before it waits, which is true of read() on a tty and of
// wait4() but not of, say, a pipe write - that would need to keep kernel
// state across the wait, i.e. to sleep on its own stack.
//
// Signals turned out not to need that. A handler runs in ring 3 like any
// other code, and rt_sigreturn restores the saved context wholesale rather
// than resuming a kernel call, so delivery is just a rewrite of the trap
// frame at the same boundary (see the signal section of process.cpp). What
// a rewound syscall gains is a choice: restart as before with SA_RESTART,
// or step back over the rewind and fail with EINTR.
//
// The stack belongs to the process table *slot*, not to the process:
// terminate() can free a process while executing on that very stack, so the
// frames go back to the PMM only at session teardown, from the console
// stack.

namespace process
{
    // Called once at boot, after the FPU is enabled.
    void init();

    // Console entry point. Loads `path` as the root process of a new session,
    // runs the session until every process in it has exited (or Esc is
    // pressed), then tears it down. argv[0] should be the program name.
    // Returns false if the program could not be started; otherwise stores the
    // root process's exit status in *exit_status.
    bool run(const char* path, int argc, const char* const* argv, int* exit_status);

    // --- Hooks from the trap entry points --------------------------------

    // Process and memory syscalls. Arguments come from regs (rdi, rsi, rdx,
    // r10, r8, r9), the result goes into regs->rax as sint64_t: >= 0 on
    // success, -errno on failure. Each may switch to another process by
    // rewriting regs/iret; they always return normally to the dispatcher.
    void sys_exit      (user_regs* regs, iret_frame* iret);
    void sys_exit_group(user_regs* regs, iret_frame* iret);
    void sys_read_key  (user_regs* regs, iret_frame* iret);
    void sys_brk       (user_regs* regs, iret_frame* iret);
    void sys_getpid    (user_regs* regs, iret_frame* iret);
    void sys_getppid   (user_regs* regs, iret_frame* iret);
    void sys_fork      (user_regs* regs, iret_frame* iret);
    void sys_execve    (user_regs* regs, iret_frame* iret);
    void sys_wait4     (user_regs* regs, iret_frame* iret);
    void sys_yield     (user_regs* regs, iret_frame* iret);
    void sys_nanosleep (user_regs* regs, iret_frame* iret);
    void sys_kill      (user_regs* regs, iret_frame* iret);
    void sys_mmap      (user_regs* regs, iret_frame* iret);
    void sys_munmap    (user_regs* regs, iret_frame* iret);
    void sys_mprotect  (user_regs* regs, iret_frame* iret);

    // Signals.
    void sys_rt_sigaction  (user_regs* regs, iret_frame* iret);
    void sys_rt_sigprocmask(user_regs* regs, iret_frame* iret);
    void sys_rt_sigpending (user_regs* regs, iret_frame* iret);
    void sys_rt_sigreturn  (user_regs* regs, iret_frame* iret);
    void sys_rt_sigsuspend (user_regs* regs, iret_frame* iret);
    void sys_pause         (user_regs* regs, iret_frame* iret);

    // Process groups (job control) and identity.
    void sys_setpgid   (user_regs* regs, iret_frame* iret);
    void sys_getpgid   (user_regs* regs, iret_frame* iret);
    void sys_getpgrp   (user_regs* regs, iret_frame* iret);
    void sys_setsid    (user_regs* regs, iret_frame* iret);
    void sys_getuid    (user_regs* regs, iret_frame* iret);
    void sys_getgid    (user_regs* regs, iret_frame* iret);
    void sys_geteuid   (user_regs* regs, iret_frame* iret);
    void sys_getegid   (user_regs* regs, iret_frame* iret);

    // Status encoding for terminate(): Linux wait(2) format.
    inline int exit_code_status(int code) { return (code & 0xFF) << 8; }
    inline int signal_status(int sig)     { return sig & 0x7F; }

    // First step of every syscall: the saved context is about to be
    // superseded, so a rewind recorded by a blocking call is no longer
    // outstanding (see `rewound` in process.cpp).
    void syscall_enter();

    // Last step of every syscall: honours a pending Esc, applies signals
    // and may switch to another process.
    void syscall_return(user_regs* regs, iret_frame* iret);

    // Is the caller in the terminal's foreground group? The tty read path
    // asks before handing input to a background job.
    bool  in_foreground();
    pid_t cur_pgrp();

    // Post a signal to every process of a group, as the tty does for ^C.
    // Returns the number of processes signalled.
    int  signal_pgrp(pid_t pgid, int sig);

    // --- Hooks for the file-descriptor syscalls (sys_fs.cpp) ---------------
    // Valid only while a session runs (syscall context). fd_table and vnode
    // are declared in fs/file.h and fs/vfs.h; forward-declared here so this
    // header stays independent.
    fd_table* cur_fds();
    vnode*    cur_cwd();                // not referenced: the process owns it
    void      set_cwd(vnode* v);        // takes one reference
    uint32_t  cur_umask();
    void      set_umask(uint32_t m);

    // Block the current process until input may be available, rewinding RIP
    // over the `int 0x80` so the syscall re-executes on wake (tty reads).
    void block_on_input(user_regs* regs, iret_frame* iret);

    // An IRQ arrived while ring 3 was running (EOI already sent).
    void on_user_interrupt(uint8_t irq, user_regs* regs, iret_frame* iret);

    // A CPU exception was raised in ring 3.
    void on_user_fault(uint64_t vector, user_regs* regs, iret_frame* iret);

    // True while a session is running (for diagnostics).
    bool active();
}

#endif // PROCESS_H
