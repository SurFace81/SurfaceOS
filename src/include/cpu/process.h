#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "paging.h"
#include "../drivers/keyboard.h"
#include "../../sdk/include/abi/process.h"

// Opaque handles defined in fs/file.h and fs/vfs.h.
struct fd_table;
struct vnode;

// ---------------------------------------------------------------------------
// User address space layout (inside PML4[USER_PML4_INDEX], 1 GiB)
// ---------------------------------------------------------------------------
//
//   USER_BASE + 1 MiB     ELF image (PT_LOAD segments, see src/sdk/linker.ld)
//   image_end             heap, grows up via SYS_BRK      (up to USER_MMAP_BASE)
//   USER_MMAP_BASE        anonymous mmap region           (up to the stack)
//   stack bottom          stack, grows down, NX           (one guard page above)
//
// Everything must stay inside [USER_BASE, USER_LIMIT); paging::map_user_page
// enforces that.
//
// The argv/envp/auxv block execve() builds lives at the top of this same
// stack (the SysV ABI initial-process-stack layout), so there is no separate
// args region any more.
#define USER_IMAGE_VADDR    (USER_BASE + 0x100000)
#define USER_IMAGE_MAX      (64 * 1024 * 1024)      // largest executable file
#define USER_MMAP_BASE      (USER_BASE + 0x10000000)
#define USER_STACK_SIZE     (1024 * 1024)           // 1 MiB: args + program stack
#define USER_STACK_TOP      (USER_LIMIT - PAGE_SIZE_4K)
// The mmap region runs up to the bottom of the stack (leaving a gap so the
// two never collide): stack bottom is USER_STACK_TOP - USER_STACK_SIZE.
#define USER_MMAP_LIMIT     (USER_STACK_TOP - USER_STACK_SIZE)

#define KERNEL_STACK_SIZE   (64 * 1024)
#define MAX_PROCESSES       32

// General purpose registers in the order SAVE_REGS pushes them (interrupts.asm):
// rax is pushed first, so it sits at the highest address.
struct user_regs
{
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp;
    uint64_t rdi, rsi, rdx, rcx, rbx, rax;
};

// What the CPU pushes when it enters the kernel from ring 3.
struct iret_frame
{
    uint64_t rip, cs, rflags, rsp, ss;
};

// A complete user-mode CPU state. Laid out exactly like the stack after
// SAVE_REGS in syscall_entry, so process_enter_user can pop it directly.
struct cpu_context
{
    user_regs  regs;
    iret_frame iret;
};

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
// switch. The restart trick above only works while a syscall has committed
// no side effect before it waits, which is true of read() on a tty and of
// wait4() but not of a pipe write or of a signal handler return - those need
// to keep kernel state across the wait, i.e. to sleep on their own stack.
// Retrofitting per-process stacks after signals exist would be far more
// painful than paying for them now, so the stack is already in place; what
// is still missing is the blocking machinery that would use it.
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

    // Status encoding for terminate(): Linux wait(2) format.
    inline int exit_code_status(int code) { return (code & 0xFF) << 8; }
    inline int signal_status(int sig)     { return sig & 0x7F; }

    // Last step of every syscall: honours a pending Esc.
    void syscall_return(user_regs* regs, iret_frame* iret);

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
