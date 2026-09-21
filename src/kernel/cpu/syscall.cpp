// Syscall dispatch (int 0x80), table-driven.
//
// ABI: arguments in rdi, rsi, rdx, r10, r8, r9 (Linux order). The result in
// rax is sint64_t: >= 0 on success, -errno on failure (abi/errno.h). An
// unknown number returns -ENOSYS.
//
// Rule for this file: a register that holds a user pointer is never
// dereferenced. Everything crosses the boundary through uaccess, which
// checks the address range and the page permissions first. Sizes are capped
// so a hostile argument cannot make the kernel allocate or loop without
// bound. Errors never kill the app.

#include "../../include/cpu/syscall.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/process.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/rtc.h"
#include "../../include/mm/heap.h"
#include "../../include/cpu/sys_fs.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/time.h"

// syscall_entry (interrupts.asm) pushes the GPRs right below the CPU's iret
// frame, so the two structs are contiguous on the kernel stack.
typedef user_regs syscall_regs;

namespace
{
    // Handlers write their result (>= 0 or -errno) into regs->rax. Blocking
    // ones may also rewrite regs/iret to switch processes; they always
    // return normally to the dispatcher.
    typedef void (*syscall_handler)(syscall_regs*, iret_frame*);

    // Linux x86_64 occupies 0..335; SurfaceOS extensions live at 0x1000+.
    const uint32_t SYSCALL_NR_MAX  = 336;
    const uint32_t SYSCALLX_NR_MAX = 16;
    const uint64_t SYSCALLX_BASE   = 0x1000;

    syscall_handler handlers[SYSCALL_NR_MAX];
    syscall_handler xhandlers[SYSCALLX_NR_MAX];

    // Caps on user-controlled sizes.
    const uint64_t MAX_WRITE_BYTES = 64 * 1024;      // one SYS_WRITE call
    const uint64_t MAX_FILE_BYTES  = 16 * 1024 * 1024;
    const uint32_t MAX_DIR_ENTRIES = 256;

    inline void set_errno(syscall_regs* regs, int err)
    {
        regs->rax = (uint64_t)(sint64_t)-err;
    }

    // Copy a path argument into kernel memory. 0 on success, -errno in *err.
    int fetch_path(uint64_t user_ptr, char* out, uint64_t max)
    {
        sint64_t r = uaccess::strncpy_from_user(out, user_ptr, max);
        if (r == -1)
            return -EFAULT;
        if (r == -2)
            return -ENAMETOOLONG;
        return 0;
    }

    void sys_clear(syscall_regs* regs, iret_frame*)
    {
        screen::clear();
        regs->rax = 0;
    }

    void sys_set_cursor(syscall_regs* regs, iret_frame*)
    {
        screen::set_cursor((uint32_t)regs->rdi, (uint32_t)regs->rsi);
        regs->rax = 0;
    }

    // -----------------------------------------------------------------------
    // Time (transitional: replaced by clock_gettime in stage 4)
    // -----------------------------------------------------------------------

    void sys_uptime(syscall_regs* regs, iret_frame*)
    {
        if (!uaccess::writable(regs->rdi, sizeof(uptime_t)))
        {
            set_errno(regs, EFAULT);
            return;
        }

        uint64_t ms = pit::uptime_ms();
        uint64_t total_sec = ms / 1000;

        uptime_t out;
        out.total_ms = ms;
        out.seconds  = (uint32_t)(total_sec % 60);
        out.minutes  = (uint32_t)((total_sec / 60) % 60);
        out.hours    = (uint32_t)(total_sec / 3600);

        if (!uaccess::copy_to_user(regs->rdi, &out, sizeof(out)))
        {
            set_errno(regs, EFAULT);
            return;
        }
        regs->rax = 0;
    }

    void sys_time(syscall_regs* regs, iret_frame*)
    {
        if (!uaccess::writable(regs->rdi, sizeof(datetime_t)))
        {
            set_errno(regs, EFAULT);
            return;
        }

        rtc_time t;
        rtc::read(&t);

        datetime_t out;
        out.year    = t.year;
        out.month   = t.month;
        out.day     = t.day;
        out.hours   = t.hours;
        out.minutes = t.minutes;
        out.seconds = t.seconds;
        out.weekday = t.weekday;

        if (!uaccess::copy_to_user(regs->rdi, &out, sizeof(out)))
        {
            set_errno(regs, EFAULT);
            return;
        }
        regs->rax = 0;
    }
} // anonymous namespace

namespace syscall
{
    void set_handler(uint32_t nr, syscall_handler_t h)
    {
        if (nr < SYSCALL_NR_MAX)
            handlers[nr] = (syscall_handler)h;
    }

    // Fill the dispatch tables. Called once from kmain before the first
    // session can run.
    void init()
    {
        for (uint32_t i = 0; i < SYSCALL_NR_MAX; i++)
            handlers[i] = nullptr;
        for (uint32_t i = 0; i < SYSCALLX_NR_MAX; i++)
            xhandlers[i] = nullptr;

        // Linux x86_64 numbers.
        handlers[SYS_EXIT]         = process::sys_exit;
        handlers[SYS_EXIT_GROUP]   = process::sys_exit_group;
        handlers[SYS_BRK]          = process::sys_brk;
        handlers[SYS_MMAP]         = process::sys_mmap;
        handlers[SYS_MPROTECT]     = process::sys_mprotect;
        handlers[SYS_MUNMAP]       = process::sys_munmap;
        handlers[SYS_SCHED_YIELD]  = process::sys_yield;
        handlers[SYS_NANOSLEEP]    = process::sys_nanosleep;
        handlers[SYS_GETPID]       = process::sys_getpid;
        handlers[SYS_GETPPID]      = process::sys_getppid;
        handlers[SYS_FORK]         = process::sys_fork;
        handlers[SYS_EXECVE]       = process::sys_execve;
        handlers[SYS_WAIT4]        = process::sys_wait4;
        handlers[SYS_KILL]         = process::sys_kill;

        // SurfaceOS extensions (legacy; replaced by POSIX interfaces later).
        xhandlers[SYSX_READ_KEY  - SYSCALLX_BASE] = process::sys_read_key;
        xhandlers[SYSX_SET_CURSOR - SYSCALLX_BASE] = sys_set_cursor;
        xhandlers[SYSX_CLEAR     - SYSCALLX_BASE] = sys_clear;
        xhandlers[SYSX_UPTIME    - SYSCALLX_BASE] = sys_uptime;
        xhandlers[SYSX_TIME      - SYSCALLX_BASE] = sys_time;

        // File-descriptor syscalls (stage 3.6).
        sys_fs::register_handlers();
    }
}

extern "C" void syscall_dispatch(syscall_regs* regs)
{
    iret_frame* iret = (iret_frame*)(regs + 1);
    uint64_t nr = regs->rax;

    syscall_handler h = nullptr;
    if (nr < SYSCALL_NR_MAX)
        h = handlers[nr];
    else if (nr >= SYSCALLX_BASE && nr < SYSCALLX_BASE + SYSCALLX_NR_MAX)
        h = xhandlers[nr - SYSCALLX_BASE];

    if (h)
        h(regs, iret);
    else
        regs->rax = (uint64_t)(sint64_t)-ENOSYS;

    process::syscall_return(regs, iret);
}
