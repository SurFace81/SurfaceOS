#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "context.h"
#include "paging.h"
#include "../drivers/keyboard.h"
#include "../../sdk/include/sfos/status.h"

// Defined in obj/object.h and fs/vfs.h.
struct handle_table;
struct vnode;

// ---------------------------------------------------------------------------
// User address space layout (lower half, [USER_MIN, USER_LIMIT), see paging.h)
// ---------------------------------------------------------------------------
//
//   0x0000000000000000  NULL guard, never mapped            (below USER_MIN)
//   0x0000000000400000  ELF image (PT_LOAD segments, see src/sdk/sfos.ld)
//   0x0000000010000000  pages of SfMemory AllocatePages     (USER_MMAP_SIZE)
//   ...                 unused
//   0x00007FFF00000000  SDK runtime code, R+X, shared   (abi/sdkimage.h)
//   0x00007FFF00100000  SDK start info, R
//   0x00007FFF00200000  SDK runtime variables, RW
//   ...                 unused
//   stack bottom        stack, grows down, NX               (USER_STACK_SIZE)
//   0x00007FFFFFFFF000  USER_STACK_TOP; one unmapped guard page above it
//
// Every address space has the whole lower half to itself, so all programs
// link at the same address. The regions are sized independently: the page
// window is a fixed span because AllocatePages scans it page by page, and the
// stack sits at the top of the half so either can grow without moving the
// other. paging::map_user_page enforces [USER_MIN, USER_LIMIT).
//
// A program starts with an empty stack in the SDK runtime, mapped at
// SDK_CODE_ADDRESS and above (abi/sdkimage.h, sdkpage.h).
#define USER_IMAGE_VADDR    0x400000ULL             // must match src/sdk/sfos.ld
#define USER_IMAGE_MAX      (64 * 1024 * 1024)      // largest executable file
#define USER_MMAP_BASE      0x10000000ULL
#define USER_MMAP_SIZE      0x30000000ULL           // 768 MiB
#define USER_MMAP_LIMIT     (USER_MMAP_BASE + USER_MMAP_SIZE)
#define USER_STACK_SIZE     (1024 * 1024)           // 1 MiB: args + program stack
#define USER_STACK_TOP      (USER_LIMIT - PAGE_SIZE_4K)

#define KERNEL_STACK_SIZE   (64 * 1024)
#define MAX_PROCESSES       64
#define MAX_THREADS         128
#define THREAD_STACK_SIZE   (256 * 1024)            // a created thread's user stack
#define MAX_ROOTS           16      // data, tmp and argN of one process

// ---------------------------------------------------------------------------
// Scheduling model
// ---------------------------------------------------------------------------
//
// A process is a running program: address space, handles, roots, screen.
// What runs are its threads, and the scheduler picks threads.
//
// Every CPU schedules on its own: each thread belongs to a CPU - the least
// busy one when it was made - and each CPU runs its own threads round
// robin, 10 ms apiece. Threads of one process may run on several CPUs at
// once: a change that shrinks an address space flushes the TLBs of the
// other CPUs that have it loaded (smp::flush_tlb). Only one CPU at a time
// runs kernel code (the big kernel lock, spinlock.h); user code runs on
// all of them at once. A request to end a process is acted on by its home
// CPU only. When a process ends while one of its threads runs on another CPU,
// that thread ends at its next kernel entry, and the address space goes
// with the last thread.
//
// User code is preempted by the timer; kernel code is not. Every thread
// has its own kernel stack and a task (task.h) that runs on it: a trap from
// ring 3 lands on that stack, and a switch is a task::switch_to from inside
// the trap handler, which leaves the trap frame where it is. The other
// thread resumes inside its own handler and returns to ring 3 through its
// own frame. Switches happen only on the way back to ring 3 -
// on return from a syscall, an IRQ that interrupted user code, or a
// user-mode fault - so the drivers (xHCI, FAT32, screen) never see
// reentrancy.
//
// The console, CMD.BIN, is a program like any other on every screen, kept
// running by cmdkeeper, a kernel process (no user address space). When
// nothing can run, a CPU's idle task - its boot task, once the kernel is
// up - halts until an interrupt wakes somebody; every CPU's own timer wakes
// it at least every tick to look for work.
//
// A call that has to wait (ReadLine, Wait, Sleep) sleeps in the kernel on a
// wait queue (wait.h) and carries on from where it was when it is woken.
//
// The stack belongs to the thread table *slot*, not to the thread:
// terminate() can free a thread while executing on that very stack, so the
// frames go back to the PMM later, from another task.

struct SfProcessInfo;           // sfos/admin.h
struct SfProcessStats;

namespace process
{
    // Called once at boot, after the FPU is enabled. The running boot
    // context becomes the idle task.
    void init();

    // Keep CMD.BIN (sfos/), the console, running on every shown screen:
    // started once the boot code calls idle(), started again whenever one
    // ends - a crash or Ctrl+Alt+C - while the programs it started run on.
    void start_cmdkeeper();

    // A kernel process (no user address space) running entry(nullptr): a
    // driver's work that has to wait, off the interrupt path. It sleeps on
    // wait queues (wait.h) like any thread.
    struct Process;
    Process* start_kernel_process(const char* name, void (*entry)(void*));

    // The rest of the boot task's life: run whatever can run, halt when
    // nothing can. Never returns.
    __attribute__((noreturn)) void idle();

    // Every other CPU, once it is set up: its boot context becomes its
    // idle task, and it runs threads from here on. Never returns.
    __attribute__((noreturn)) void run_cpu();

    // Handles to processes. The object behind one outlives the process, so
    // the exit status stays readable after the process is gone.
    //
    // open: put a handle to live process `pid` into t (lowest free slot).
    //   0, -ESRCH (no such process), -EMFILE.
    sint64_t open(handle_table* t, pid_t pid, sint32_t* out);

    // What a program in SF_CONSOLE_LINE prints: into its log, when it runs
    // in the background (sfconsole.cpp).
    void log_output(const char* s, uint64_t len);

    // --- Hooks from the trap entry points --------------------------------

    // The process calls of the SDK (sfcall.cpp). Arguments come from regs
    // (rdi, rsi, rdx, r10, r8, r9), the SfStatus goes into regs->rax. Each
    // may switch to another process by rewriting regs/iret; they always
    // return normally to the dispatcher.
    //
    // SFCALL_EXIT (SfStatus): the SurfaceOS ABI's exit.
    void sf_exit       (user_regs* regs, iret_frame* iret);
    // SFCALL_THREAD_CREATE / EXIT / JOIN: SfStatus results.
    void sf_thread_create(user_regs* regs, iret_frame* iret);
    void sf_thread_exit(user_regs* regs, iret_frame* iret);
    void sf_thread_join(user_regs* regs, iret_frame* iret);
    // SFCALL_PROCESS_START / WAIT / GET_ID / GET_ARGS: SfStatus results.
    void sf_process_start(user_regs* regs, iret_frame* iret);
    void sf_process_wait(user_regs* regs, iret_frame* iret);
    void sf_process_id_of(user_regs* regs, iret_frame* iret);
    void sf_get_id     (user_regs* regs, iret_frame* iret);
    void sf_get_args   (user_regs* regs, iret_frame* iret);
    // SFCALL_MEMORY_ALLOCATE_PAGES / FREE_PAGES: SfStatus results.
    void sf_allocate_pages(user_regs* regs, iret_frame* iret);
    void sf_free_pages (user_regs* regs, iret_frame* iret);

    // First step of every call: a thread whose process ended on another
    // CPU meanwhile ends here instead (no return).
    void syscall_enter(user_regs* regs, iret_frame* iret);

    // Last step of every call: acts on Ctrl+Alt+C / Ctrl+Alt+Z and end
    // requests, and may switch to another process.
    void syscall_return(user_regs* regs, iret_frame* iret);

    // The process that gets screen `screen`'s keys (-1: none). A ReadLine
    // waits (wait_for_input) until its process is the one; false when the
    // process is to be ended.
    pid_t screen_input_owner(uint32_t screen);

    // Ctrl+Alt+C: end every program on `screen` at the next scheduling
    // decision (called from the keyboard IRQ).
    void end_screen_programs(uint32_t screen);
    // Ctrl+Alt+Z: pause every program on `screen`, or let them go on when
    // they are paused; the same way.
    void pause_screen_programs(uint32_t screen);

    // The admin right (sfos/admin.h, sfadmin.cpp): has the caller got it;
    // the running programs, up to `max` of them into `out` (returns how
    // many there are); end program `pid` as Ctrl+Alt+C would (false: no
    // such program).
    bool     current_admin();
    uint64_t list_programs(SfProcessInfo* out, uint64_t max);
    bool     end_program(pid_t pid);
    // fg and bg: program `pid` - with everything else on its screen but
    // the console - moves to the caller's screen and gets its keys, or to a
    // hidden one of its own with a log; paused, it goes on.
    SfStatus move_to_foreground(pid_t pid);
    SfStatus move_to_background(pid_t pid);

    // The calling process's screen, and its console mode (sfconsole.cpp).
    uint32_t current_screen();
    bool     console_raw();
    void     set_console_raw(bool raw);
    // The caller's ReadLine history: `size` zeroed bytes the first time,
    // the same block afterwards, freed with the process. nullptr when out
    // of memory.
    void*    line_history(uint64_t size);
    bool  wait_for_input();
    // Has the caller lost screen `screen`'s input, or left the screen?
    bool  input_changed(uint32_t screen);

    // The caller's handles, and the directory behind its root `name`
    // ("data", "tmp") or nullptr - not referenced: the process owns it.
        handle_table* cur_handles();
    vnode*    cur_root(const char* name);

    // Every timer tick, from any ring: wakes the sleep_until sleepers that
    // are due.
    void on_timer_tick();

    // Every tick of every CPU, before anything else (no lock needed): what
    // it ran gets the tick. cpu_times: ms CPU `cpu` has run and worked;
    // program_stats: what program `pid` uses (sfos/admin.h; false: no such
    // program).
    void account_tick();
    void cpu_times(uint32_t cpu, uint64_t* busy_ms, uint64_t* total_ms);
    bool program_stats(pid_t pid, SfProcessStats* out);

    // An IRQ arrived while ring 3 was running (EOI already sent).
    void on_user_interrupt(uint8_t irq, user_regs* regs, iret_frame* iret);

    // A CPU exception was raised in ring 3.
    void on_user_fault(uint64_t vector, user_regs* regs, iret_frame* iret);

}

#endif // PROCESS_H
