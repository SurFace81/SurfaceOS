// Processes: table, ELF program loading, the scheduler, and the process and
// memory syscalls. See process.h for the scheduling model.
//
// User memory is W^X throughout: code is read+execute, everything writable
// (data, heap, stack) is NX, and the kernel-written pages (program_info, argv)
// are read-only. mmap/mprotect are the one sanctioned way to get RWX memory,
// which a JIT or tcc -run needs.

#include "../../include/cpu/process.h"
#include "../../include/cpu/tss.h"
#include "../../include/cpu/elf.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/irq.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"

extern "C" void process_enter_user(cpu_context* ctx);
extern "C" void process_return_to_kernel(void);

#define USER_CS             0x23
#define USER_SS             0x2B
#define RFLAGS_USER         0x202       // IF + reserved bit 1
#define INT80_LENGTH        2           // `int $0x80` is CD 80
#define TIME_SLICE_TICKS    10          // PIT ticks (~10 ms at 1 kHz)
#define LINE_CAPACITY       256

// Syscall failure: -errno in rax (Linux convention, see abi/errno.h).
#define SYSCALL_ERR(e)      ((uint64_t)(sint64_t)-(e))

namespace process
{
    // -----------------------------------------------------------------------
    // Process table
    // -----------------------------------------------------------------------

    enum class State : uint8_t { Unused, Runnable, Blocked, Zombie };
    enum class Wait  : uint8_t { None, Key, Child, Sleep };

    struct Process
    {
        State       state;
        Wait        wait;
        pid_t       pid;
        pid_t       ppid;           // 0: parent is the console session
        pid_t       wait_pid;       // Wait::Child: which child (<= 0: any)
        uint64_t    wake_tick;      // Wait::Sleep
        int         exit_status;    // valid in Zombie

        char        name[32];

        uint64_t    cr3;
        uint64_t    brk_start;      // end of the ELF image
        uint64_t    brk;            // current program break
        uint64_t    mmap_cursor;    // where the next mmap search starts

        cpu_context ctx;            // user state while not running
        uint8_t     fpu[512] __attribute__((aligned(16)));   // FXSAVE area

        // SYS_READ_LINE edits across restarts, so its buffer lives here.
        char        line[LINE_CAPACITY];
        uint32_t    line_pos;
    };

    static Process  table[MAX_PROCESSES];
    static Process* current   = nullptr;
    static uint32_t last_slot = 0;
    static pid_t    next_pid  = 1;

    // Session state
    static bool     session_active  = false;
    static volatile bool kill_requested = false;
    static pid_t    root_pid        = 0;
    static int      root_status     = 0;
    static uint32_t slice_ticks     = 0;

    // Single kernel stack for every trap from ring 3 (TSS RSP0).
    static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

    // Clean FPU/SSE state every new program starts from.
    static uint8_t fpu_template[512] __attribute__((aligned(16)));

    static inline uint64_t read_cr3()
    {
        uint64_t v;
        asm volatile("mov %%cr3, %0" : "=r"(v));
        return v;
    }

    static inline uint64_t kernel_stack_top()
    {
        return (uint64_t)kernel_stack + KERNEL_STACK_SIZE;
    }

    static void copy_bytes(uint8_t* dst, const uint8_t* src, uint64_t n)
    {
        for (uint64_t i = 0; i < n; i++)
            dst[i] = src[i];
    }

    static void copy_name(char* dst, const char* path)
    {
        const char* name = path;
        for (const char* p = path; *p; p++)
            if (*p == '\\' || *p == '/')
                name = p + 1;

        uint32_t i = 0;
        for (; name[i] && i < sizeof(Process::name) - 1; i++)
            dst[i] = name[i];
        dst[i] = '\0';
    }

    static Process* alloc_process()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            if (table[i].state != State::Unused)
                continue;

            Process* p = &table[i];
            memory::memset((uint8_t*)p, 0x00, sizeof(Process));
            p->pid = next_pid++;
            if (next_pid <= 0)
                next_pid = 1;
            return p;
        }
        return nullptr;
    }

    static void free_process(Process* p)
    {
        p->state = State::Unused;
        p->pid = 0;
    }

    static Process* find_live(pid_t pid)
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->pid == pid && (p->state == State::Runnable || p->state == State::Blocked))
                return p;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Keyboard queue (shared by the session) and Esc
    // -----------------------------------------------------------------------

    static const uint32_t KEY_BUF_SIZE = 64;
    static keyboard_event_t key_buf[KEY_BUF_SIZE];
    static volatile uint32_t key_head = 0;
    static volatile uint32_t key_tail = 0;

    // Runs in the keyboard IRQ.
    static void session_key_handler(keyboard_event_t e)
    {
        // Esc belongs to the kernel while a session runs: it terminates every
        // process of the session, whatever they are doing - spinning in user
        // code, blocked in a syscall, or waiting on each other. The request is
        // acted on at the next return to ring 3 (see reschedule()).
        if (e.KeyCode == KEY_ESCAPE)
        {
            if (e.type == KEY_PRESS)
                kill_requested = true;
            return;
        }

        uint32_t next = (key_head + 1) % KEY_BUF_SIZE;
        if (next == key_tail)
            return;     // full, drop

        key_buf[key_head] = e;
        key_head = next;
    }

    static bool has_key()
    {
        return key_head != key_tail;
    }

    static keyboard_event_t pop_key()
    {
        keyboard_event_t e = key_buf[key_tail];
        key_tail = (key_tail + 1) % KEY_BUF_SIZE;
        return e;
    }

    // -----------------------------------------------------------------------
    // Contexts
    // -----------------------------------------------------------------------

    static void fpu_save(uint8_t* area)    { asm volatile("fxsave (%0)"  :: "r"(area) : "memory"); }
    static void fpu_restore(uint8_t* area) { asm volatile("fxrstor (%0)" :: "r"(area) : "memory"); }

    static void save_context(Process* p, user_regs* regs, iret_frame* iret)
    {
        p->ctx.regs = *regs;
        p->ctx.iret = *iret;
        fpu_save(p->fpu);
    }

    // Make `p` the running process: its registers go into the trap frame that
    // is about to be popped, its address space and FPU state become live.
    static void load_context(Process* p, user_regs* regs, iret_frame* iret)
    {
        current = p;
        paging::switch_address_space(p->cr3);
        fpu_restore(p->fpu);
        *regs = p->ctx.regs;
        *iret = p->ctx.iret;
    }

    static void initial_context(cpu_context* ctx, uint64_t entry)
    {
        memory::memset((uint8_t*)ctx, 0x00, sizeof(cpu_context));
        ctx->regs.rdi    = USER_INFO_VADDR;         // program_info* for _start
        ctx->iret.rip    = entry;
        ctx->iret.cs     = USER_CS;
        ctx->iret.rflags = RFLAGS_USER;
        // The SysV ABI wants rsp % 16 == 8 at function entry (as if a call
        // had just pushed a return address). _start is an ordinary function.
        ctx->iret.rsp    = USER_STACK_TOP - 8;
        ctx->iret.ss     = USER_SS;
    }

    // -----------------------------------------------------------------------
    // Program loading
    // -----------------------------------------------------------------------

    struct ArgList
    {
        int          argc;
        uint32_t     used;                              // bytes in data
        uint32_t     offset[ARG_MAX_COUNT];             // into data
        char         data[ARG_MAX_BYTES];
    };

    static bool args_push(ArgList* a, const char* s)
    {
        if (a->argc >= ARG_MAX_COUNT)
            return false;

        uint32_t len = 0;
        while (s[len])
            len++;

        if (a->used + len + 1 > ARG_MAX_BYTES)
            return false;

        a->offset[a->argc++] = a->used;
        copy_bytes((uint8_t*)a->data + a->used, (const uint8_t*)s, len + 1);
        a->used += len + 1;
        return true;
    }

    struct Image
    {
        uint64_t cr3;
        uint64_t entry;
        uint64_t image_end;
    };

    // Map [vaddr, vaddr+size) with zeroed 4 KiB user pages (active space).
    static bool map_user_region(uint64_t vaddr, uint64_t size, uint64_t flags)
    {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE_4K)
        {
            uint64_t frame = pmm::alloc_frame();
            if (!frame)
                return false;

            memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
            if (!paging::map_user_page(vaddr + off, frame, flags))
            {
                pmm::free_frame(frame);
                return false;
            }
        }
        return true;
    }

    // Write kernel data into a user page of the active address space through
    // the identity map (the pages may be read-only to the app).
    static void poke_user(uint64_t vaddr, const uint8_t* src, uint64_t len)
    {
        while (len)
        {
            uint64_t phys  = paging::page_frame(vaddr & ~(PAGE_SIZE_4K - 1));
            uint64_t off   = vaddr & (PAGE_SIZE_4K - 1);
            uint64_t chunk = PAGE_SIZE_4K - off;
            if (chunk > len)
                chunk = len;

            copy_bytes((uint8_t*)(phys + off), src, chunk);
            vaddr += chunk;
            src   += chunk;
            len   -= chunk;
        }
    }

    // Read the whole executable into kernel memory, sized from its directory
    // entry. Returns the buffer or nullptr with *out_err set to -errno.
    static uint8_t* read_executable(const char* path, uint32_t* out_size, int* out_err)
    {
        fat32_dir_entry entry;
        if (!fat32::resolve_path_pub(path, &entry))
        {
            *out_err = ENOENT;
            return nullptr;
        }

        if (entry.attr & FAT32_ATTR_DIRECTORY)
        {
            *out_err = EISDIR;
            return nullptr;
        }

        if (entry.file_size == 0)
        {
            *out_err = ENOEXEC;
            return nullptr;
        }
        if (entry.file_size > USER_IMAGE_MAX)
        {
            uart::printf("process: %s has an unusable size (%u bytes)\n",
                         path, entry.file_size);
            *out_err = EFBIG;
            return nullptr;
        }

        uint8_t* image = (uint8_t*)kmalloc(entry.file_size);
        if (!image)
        {
            *out_err = ENOMEM;
            return nullptr;
        }

        uint32_t read = fat32::read_file(path, image, entry.file_size);
        if (read == (uint32_t)-1 || read == 0)
        {
            kfree(image);
            *out_err = EIO;
            return nullptr;
        }

        *out_err = 0;
        *out_size = read;
        return image;
    }

    // Build a complete address space for `path`: ELF segments, stack,
    // program_info and argv. The active address space is unchanged on return.
    // Returns 0 or -errno.
    static sint64_t load_program(const char* path, const ArgList* args, Image* out)
    {
        int err = 0;
        uint32_t size = 0;
        uint8_t* file = read_executable(path, &size, &err);
        if (!file)
            return -(sint64_t)err;

        if (!elf::is_elf(file, size))
        {
            uart::printf("process: %s is not an ELF64 executable\n", path);
            kfree(file);
            return -ENOEXEC;
        }

        uint64_t as = paging::create_address_space();
        if (!as)
        {
            kfree(file);
            return -ENOMEM;
        }

        uint64_t prev = read_cr3();
        paging::switch_address_space(as);

        elf::LoadResult lr = {0, 0, false};
        bool ok =
            map_user_region(USER_INFO_VADDR, PAGE_SIZE_4K, PAGE_NX) &&
            map_user_region(USER_ARGS_VADDR, USER_ARGS_PAGES * PAGE_SIZE_4K, PAGE_NX) &&
            map_user_region(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                            PAGE_WRITE | PAGE_NX);
        if (!ok)
            err = ENOMEM;
        else
        {
            lr = elf::load(file, size);
            ok = lr.valid && lr.image_end <= USER_MMAP_BASE;
            if (!ok)
                err = ENOEXEC;
        }

        if (ok)
        {
            // argv: pointer array first, then the strings.
            uint64_t ptr_bytes = (uint64_t)(args->argc + 1) * sizeof(uint64_t);
            uint64_t strings   = USER_ARGS_VADDR + ptr_bytes;

            for (int i = 0; i < args->argc; i++)
            {
                uint64_t v = strings + args->offset[i];
                poke_user(USER_ARGS_VADDR + (uint64_t)i * sizeof(uint64_t),
                          (const uint8_t*)&v, sizeof(v));
            }
            uint64_t null_ptr = 0;
            poke_user(USER_ARGS_VADDR + (uint64_t)args->argc * sizeof(uint64_t),
                      (const uint8_t*)&null_ptr, sizeof(null_ptr));
            poke_user(strings, (const uint8_t*)args->data, args->used);

            program_info info;
            info.heap_start = lr.image_end;
            info.heap_size  = USER_MMAP_BASE - lr.image_end;
            info.argc       = (uint64_t)args->argc;
            info.argv       = (char**)USER_ARGS_VADDR;
            poke_user(USER_INFO_VADDR, (const uint8_t*)&info, sizeof(info));
        }

        paging::switch_address_space(prev);
        kfree(file);

        if (!ok)
        {
            paging::destroy_address_space(as);
            return -(sint64_t)err;
        }

        out->cr3       = as;
        out->entry     = lr.entry;
        out->image_end = lr.image_end;
        return 0;
    }

    static void adopt_image(Process* p, const Image* img, const char* path)
    {
        p->cr3         = img->cr3;
        p->brk_start   = img->image_end;
        p->brk         = img->image_end;
        p->mmap_cursor = USER_MMAP_BASE;
        p->line_pos    = 0;
        copy_name(p->name, path);
        initial_context(&p->ctx, img->entry);
        copy_bytes(p->fpu, fpu_template, sizeof(p->fpu));
    }

    // -----------------------------------------------------------------------
    // Scheduler
    // -----------------------------------------------------------------------

    // Terminate `p`: release its memory, reparent its children to the session
    // and leave a zombie for its parent (or nothing, if the parent is the
    // session itself).
    static void terminate(Process* p, int status)
    {
        if (p->cr3)
        {
            if (read_cr3() == p->cr3)
                paging::switch_address_space(paging::kernel_pml4());
            paging::destroy_address_space(p->cr3);
            p->cr3 = 0;
        }

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q->state == State::Unused || q->ppid != p->pid || q == p)
                continue;

            if (q->state == State::Zombie)
                free_process(q);
            else
                q->ppid = 0;
        }

        if (p->pid == root_pid)
            root_status = status;

        if (p->ppid == 0)
        {
            free_process(p);
        }
        else
        {
            p->state = State::Zombie;
            p->wait = Wait::None;
            p->exit_status = status;
        }

        if (p == current)
            current = nullptr;
    }

    static bool child_event(Process* p)
    {
        bool has_child = false;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q->state == State::Unused || q->ppid != p->pid)
                continue;
            if (p->wait_pid > 0 && q->pid != p->wait_pid)
                continue;

            if (q->state == State::Zombie)
                return true;
            has_child = true;
        }
        return !has_child;     // nothing left to wait for: let waitpid fail
    }

    static bool wake_ready(Process* p)
    {
        switch (p->wait)
        {
            case Wait::Key:   return has_key();
            case Wait::Child: return child_event(p);
            case Wait::Sleep: return pit::ticks() >= p->wake_tick;
            default:          return true;
        }
    }

    static Process* pick_next()
    {
        for (uint32_t i = 1; i <= MAX_PROCESSES; i++)
        {
            uint32_t slot = (last_slot + i) % MAX_PROCESSES;
            Process* p = &table[slot];

            if (p->state == State::Blocked && wake_ready(p))
            {
                p->state = State::Runnable;
                p->wait = Wait::None;
            }

            if (p->state == State::Runnable)
            {
                last_slot = slot;
                return p;
            }
        }
        return nullptr;
    }

    static bool any_live()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (table[i].state == State::Runnable || table[i].state == State::Blocked)
                return true;
        return false;
    }

    __attribute__((noreturn))
    static void end_session()
    {
        paging::switch_address_space(paging::kernel_pml4());
        current = nullptr;
        process_return_to_kernel();
        while (1) asm volatile("hlt");
    }

    static void kill_session()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->state == State::Runnable || p->state == State::Blocked)
                terminate(p, EXIT_ESCAPE);
        }
    }

    // Choose what runs next and load it into the trap frame. The caller has
    // already saved the current process (if it is still alive). Never returns
    // to the caller when the session is over.
    static void reschedule(user_regs* regs, iret_frame* iret)
    {
        for (;;)
        {
            if (kill_requested)
            {
                screen::printf("\n[terminated with Esc]\n");
                kill_session();
                end_session();
            }

            if (!any_live())
                end_session();

            Process* next = pick_next();
            if (next)
            {
                slice_ticks = 0;
                load_context(next, regs, iret);
                return;
            }

            // Everyone is waiting (for a key, a timer, a child). Idle until
            // an interrupt changes that. Interrupts arriving here come from
            // ring 0, so they never re-enter the scheduler.
            asm volatile("sti; hlt");
        }
    }

    // Put the current process to sleep. With `restart`, the syscall is
    // re-executed from scratch when the process wakes up; otherwise it
    // returns whatever the caller left in regs->rax.
    static void block(Wait why, bool restart, user_regs* regs, iret_frame* iret)
    {
        if (restart)
            iret->rip -= INT80_LENGTH;

        current->state = State::Blocked;
        current->wait = why;
        save_context(current, regs, iret);
        reschedule(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Session (console entry point)
    // -----------------------------------------------------------------------

    void init()
    {
        memory::memset((uint8_t*)table, 0x00, sizeof(table));

        asm volatile("fninit");
        fpu_save(fpu_template);
        // MXCSR (offset 24): all SSE exceptions masked, round to nearest.
        uint32_t mxcsr = 0x1F80;
        copy_bytes(fpu_template + 24, (const uint8_t*)&mxcsr, sizeof(mxcsr));
    }

    bool active()
    {
        return session_active;
    }

    bool run(const char* path, int argc, const char* const* argv, int* exit_status)
    {
        if (session_active)
            return false;

        ArgList args;
        memory::memset((uint8_t*)&args, 0x00, sizeof(args));
        if (argc <= 0)
        {
            args_push(&args, path);
        }
        else
        {
            for (int i = 0; i < argc; i++)
                if (!args_push(&args, argv[i]))
                    return false;
        }

        Image img;
        if (load_program(path, &args, &img) < 0)
            return false;

        Process* p = alloc_process();
        if (!p)
        {
            paging::destroy_address_space(img.cr3);
            return false;
        }
        adopt_image(p, &img, path);
        p->ppid  = 0;
        p->state = State::Runnable;

        root_pid       = p->pid;
        root_status    = 0;
        kill_requested = false;
        session_active = true;
        slice_ticks    = 0;
        last_slot      = (uint32_t)(p - table);

        key_head = key_tail = 0;
        keyboard_callback_t prev_callback = keyboard::get_callback();
        keyboard::set_keyboard_callback(session_key_handler);

        screen::hide_cursor();
        screen::clear();
        screen::draw_title_bar(p->name);
        uint32_t bar_h = screen::title_bar_height();
        screen::push_viewport(screen::vp_x(), screen::vp_y() + bar_h,
                              screen::vp_w(), screen::vp_h() - bar_h);

        uart::printf("process: session start, pid %u %s entry=%llx\n",
                     (uint32_t)p->pid, p->name, p->ctx.iret.rip);

        tss::set_kernel_stack(kernel_stack_top());

        current = p;
        paging::switch_address_space(p->cr3);
        fpu_restore(p->fpu);

        cpu_context* frame = (cpu_context*)(kernel_stack_top() - sizeof(cpu_context));
        *frame = p->ctx;
        process_enter_user(frame);

        // --- the session is over ---
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q->state == State::Unused)
                continue;
            if (q->cr3)
                paging::destroy_address_space(q->cr3);
            free_process(q);
        }

        current = nullptr;
        session_active = false;
        tss::set_kernel_stack(0);

        uart::printf("process: session end, status %u\n", (uint32_t)root_status);

        screen::pop_viewport();
        screen::clear();
        screen::show_cursor();
        keyboard::set_keyboard_callback(prev_callback);

        *exit_status = root_status;
        return true;
    }

    // -----------------------------------------------------------------------
    // Trap hooks
    // -----------------------------------------------------------------------

    void on_user_interrupt(uint8_t irq, user_regs* regs, iret_frame* iret)
    {
        if (!session_active || !current)
            return;

        if (kill_requested)
        {
            reschedule(regs, iret);     // does not return
            return;
        }

        if (irq != IRQ0_TIMER)
            return;

        if (++slice_ticks < TIME_SLICE_TICKS)
            return;

        save_context(current, regs, iret);
        reschedule(regs, iret);
    }

    void on_user_fault(uint64_t vector, user_regs* regs, iret_frame* iret)
    {
        screen::printf("\n[pid %u %s terminated: CPU exception %u]\n",
                       (uint32_t)current->pid, current->name, (uint32_t)vector);

        terminate(current, EXIT_FAULT_BASE + (int)vector);
        reschedule(regs, iret);
    }

    void syscall_return(user_regs* regs, iret_frame* iret)
    {
        if (kill_requested)
            reschedule(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Syscalls: process control
    // -----------------------------------------------------------------------

    void sys_exit(user_regs* regs, iret_frame* iret)
    {
        terminate(current, (int)(sint32_t)regs->rdi);
        reschedule(regs, iret);
    }

    void sys_getpid(user_regs* regs, iret_frame*)
    {
        regs->rax = (uint64_t)current->pid;
    }

    void sys_getppid(user_regs* regs, iret_frame*)
    {
        regs->rax = (uint64_t)current->ppid;
    }

    void sys_yield(user_regs* regs, iret_frame* iret)
    {
        regs->rax = 0;
        save_context(current, regs, iret);
        reschedule(regs, iret);
    }

    void sys_sleep(user_regs* regs, iret_frame* iret)
    {
        uint64_t ms = regs->rdi;
        regs->rax = 0;

        if (ms == 0)
        {
            sys_yield(regs, iret);
            return;
        }

        uint32_t hz = pit::real_frequency();
        if (!hz)
            hz = pit::frequency();
        uint64_t ticks = (ms * hz + 999) / 1000;
        current->wake_tick = pit::ticks() + (ticks ? ticks : 1);

        block(Wait::Sleep, false, regs, iret);
    }

    void sys_fork(user_regs* regs, iret_frame* iret)
    {
        Process* child = alloc_process();
        if (!child)
        {
            regs->rax = SYSCALL_ERR(EAGAIN);
            return;
        }

        child->cr3 = paging::create_address_space();
        if (!child->cr3 || !paging::clone_user_space(child->cr3, current->cr3))
        {
            if (child->cr3)
                paging::destroy_address_space(child->cr3);
            free_process(child);
            regs->rax = SYSCALL_ERR(ENOMEM);
            return;
        }

        copy_bytes((uint8_t*)child->name, (const uint8_t*)current->name, sizeof(child->name));
        child->ppid        = current->pid;
        child->brk_start   = current->brk_start;
        child->brk         = current->brk;
        child->mmap_cursor = current->mmap_cursor;

        // The child resumes from the same instruction with rax = 0.
        child->ctx.regs = *regs;
        child->ctx.iret = *iret;
        child->ctx.regs.rax = 0;
        fpu_save(child->fpu);

        child->state = State::Runnable;
        regs->rax = (uint64_t)child->pid;
    }

    void sys_exec(user_regs* regs, iret_frame* iret)
    {
        char path[uaccess::MAX_PATH];
        if (uaccess::strncpy_from_user(path, regs->rdi, sizeof(path)) <= 0)
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        // Copy argv into the kernel before the old address space goes away.
        ArgList args;
        memory::memset((uint8_t*)&args, 0x00, sizeof(args));
        uint64_t argv_ptr = regs->rsi;

        if (!argv_ptr)
        {
            args_push(&args, path);
        }
        else
        {
            for (int i = 0; ; i++)
            {
                uint64_t str = 0;
                if (!uaccess::copy_from_user(&str, argv_ptr + (uint64_t)i * 8, 8))
                {
                    regs->rax = SYSCALL_ERR(EFAULT);
                    return;
                }
                if (!str)
                    break;

                char arg[ARG_MAX_BYTES];
                if (uaccess::strncpy_from_user(arg, str, sizeof(arg)) < 0)
                {
                    regs->rax = SYSCALL_ERR(EFAULT);
                    return;
                }
                if (!args_push(&args, arg))
                {
                    regs->rax = SYSCALL_ERR(E2BIG);
                    return;
                }
            }
            if (args.argc == 0)
                args_push(&args, path);
        }

        Image img;
        sint64_t rc = load_program(path, &args, &img);
        if (rc < 0)
        {
            regs->rax = (uint64_t)rc;       // the old program keeps running
            return;
        }

        // Point of no return: swap in the new image.
        uint64_t old = current->cr3;
        paging::switch_address_space(img.cr3);
        paging::destroy_address_space(old);

        adopt_image(current, &img, path);
        fpu_restore(current->fpu);
        *regs = current->ctx.regs;
        *iret = current->ctx.iret;
    }

    void sys_waitpid(user_regs* regs, iret_frame* iret)
    {
        pid_t    pid        = (pid_t)(sint32_t)regs->rdi;
        uint64_t status_ptr = regs->rsi;
        uint64_t options    = regs->rdx;

        if (status_ptr && !uaccess::writable(status_ptr, sizeof(int)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        bool has_child = false;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q->state == State::Unused || q->ppid != current->pid)
                continue;
            if (pid > 0 && q->pid != pid)
                continue;

            has_child = true;
            if (q->state != State::Zombie)
                continue;

            int status = q->exit_status;
            if (status_ptr && !uaccess::copy_to_user(status_ptr, &status, sizeof(status)))
            {
                regs->rax = SYSCALL_ERR(EFAULT);
                return;
            }

            regs->rax = (uint64_t)q->pid;
            free_process(q);
            return;
        }

        if (!has_child)
        {
            regs->rax = SYSCALL_ERR(ECHILD);
            return;
        }

        if (options & WNOHANG)
        {
            regs->rax = 0;
            return;
        }

        current->wait_pid = pid;
        block(Wait::Child, true, regs, iret);
    }

    void sys_kill(user_regs* regs, iret_frame* iret)
    {
        Process* target = find_live((pid_t)(sint32_t)regs->rdi);
        if (!target)
        {
            regs->rax = SYSCALL_ERR(ESRCH);
            return;
        }

        if (target == current)
        {
            terminate(current, EXIT_KILLED);
            reschedule(regs, iret);
            return;
        }

        terminate(target, EXIT_KILLED);
        regs->rax = 0;
    }

    // -----------------------------------------------------------------------
    // Syscalls: keyboard
    // -----------------------------------------------------------------------

    void sys_read_key(user_regs* regs, iret_frame* iret)
    {
        if (!uaccess::writable(regs->rdi, sizeof(keyboard_event_t)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        if (!has_key())
        {
            block(Wait::Key, true, regs, iret);
            return;
        }

        keyboard_event_t e = pop_key();
        regs->rax = uaccess::copy_to_user(regs->rdi, &e, sizeof(e)) ? 0 : SYSCALL_ERR(EFAULT);
    }

    void sys_read_line(user_regs* regs, iret_frame* iret)
    {
        uint64_t user_buf = regs->rdi;
        uint64_t max_len  = regs->rsi;

        if (max_len == 0 || max_len > 4096)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }
        if (!uaccess::writable(user_buf, max_len))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        uint32_t limit = (uint32_t)max_len - 1;
        if (limit > LINE_CAPACITY - 1)
            limit = LINE_CAPACITY - 1;

        // Consume whatever has been typed; the line survives if we block.
        while (has_key())
        {
            keyboard_event_t e = pop_key();
            if (e.type != KEY_PRESS)
                continue;

            if (e.KeyCode == KEY_ENTER)
            {
                screen::printf("\n");
                uint32_t len = current->line_pos;
                current->line[len] = '\0';
                current->line_pos = 0;
                regs->rax = uaccess::copy_to_user(user_buf, current->line, len + 1)
                          ? (uint64_t)len : SYSCALL_ERR(EFAULT);
                return;
            }

            if (e.KeyCode == KEY_BACKSPACE)
            {
                if (current->line_pos > 0)
                {
                    current->line_pos--;
                    screen::printf("\b \b");
                }
                continue;
            }

            if (e.KeyChar && current->line_pos < limit)
            {
                current->line[current->line_pos++] = e.KeyChar;
                char tmp[2] = { e.KeyChar, 0 };
                screen::printf("%s", tmp);
            }
        }

        block(Wait::Key, true, regs, iret);
    }

    // -----------------------------------------------------------------------
    // Syscalls: memory
    // -----------------------------------------------------------------------

    static uint64_t prot_to_flags(uint64_t prot)
    {
        uint64_t flags = 0;
        if (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))
            flags |= PAGE_USER;
        if (prot & PROT_WRITE)
            flags |= PAGE_WRITE;
        if (!(prot & PROT_EXEC))
            flags |= PAGE_NX;
        return flags;
    }

    static bool range_in(uint64_t addr, uint64_t len, uint64_t lo, uint64_t hi)
    {
        return addr >= lo && addr < hi && len <= hi - addr;
    }

    static bool range_unmapped(uint64_t addr, uint64_t pages)
    {
        for (uint64_t i = 0; i < pages; i++)
            if (paging::page_frame(addr + i * PAGE_SIZE_4K))
                return false;
        return true;
    }

    // First fit over the mmap region, starting at the process's cursor.
    static uint64_t find_free_range(Process* p, uint64_t pages)
    {
        uint64_t region_pages = (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE_4K;
        uint64_t start = (p->mmap_cursor - USER_MMAP_BASE) / PAGE_SIZE_4K;
        uint64_t run = 0;

        for (uint64_t i = 0; i < region_pages; i++)
        {
            uint64_t idx = (start + i) % region_pages;
            if (idx == 0)
                run = 0;        // wrapped: runs may not cross the region end

            uint64_t v = USER_MMAP_BASE + idx * PAGE_SIZE_4K;
            if (paging::page_frame(v))
            {
                run = 0;
                continue;
            }

            if (++run == pages)
                return v - (pages - 1) * PAGE_SIZE_4K;
        }
        return 0;
    }

    static void release_range(uint64_t addr, uint64_t pages)
    {
        for (uint64_t i = 0; i < pages; i++)
        {
            uint64_t v = addr + i * PAGE_SIZE_4K;
            uint64_t phys = paging::page_frame(v);
            if (!phys)
                continue;
            paging::unmap_page(v);
            pmm::free_frame(phys);
        }
    }

    void sys_brk(user_regs* regs, iret_frame*)
    {
        uint64_t new_brk = regs->rdi;
        Process* p = current;
        regs->rax = p->brk;

        if (new_brk == 0 || new_brk < p->brk_start || new_brk > USER_MMAP_BASE)
            return;

        uint64_t new_page = (new_brk + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        uint64_t old_page = (p->brk  + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);

        if (new_page > old_page)
        {
            for (uint64_t page = old_page; page < new_page; page += PAGE_SIZE_4K)
            {
                uint64_t frame = pmm::alloc_frame();
                bool ok = frame != 0;
                if (ok)
                {
                    memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
                    ok = paging::map_user_page(page, frame, PAGE_WRITE | PAGE_NX);
                    if (!ok)
                        pmm::free_frame(frame);
                }

                if (!ok)
                {
                    // All or nothing: undo the pages this call mapped.
                    release_range(old_page, (page - old_page) / PAGE_SIZE_4K);
                    return;
                }
            }
        }
        else if (new_page < old_page)
        {
            release_range(new_page, (old_page - new_page) / PAGE_SIZE_4K);
        }

        p->brk = new_brk;
        regs->rax = new_brk;
    }

    void sys_mmap(user_regs* regs, iret_frame*)
    {
        uint64_t hint = regs->rdi;
        uint64_t len  = regs->rsi;
        uint64_t prot = regs->rdx;
        regs->rax = SYSCALL_ERR(EINVAL);

        if (len == 0 || len > USER_MMAP_LIMIT - USER_MMAP_BASE ||
            (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)))
            return;

        uint64_t pages = (len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        uint64_t size  = pages * PAGE_SIZE_4K;
        uint64_t addr  = 0;

        if (hint && !(hint & (PAGE_SIZE_4K - 1)) &&
            range_in(hint, size, USER_MMAP_BASE, USER_MMAP_LIMIT) &&
            range_unmapped(hint, pages))
            addr = hint;
        else
            addr = find_free_range(current, pages);

        if (!addr)
        {
            regs->rax = SYSCALL_ERR(ENOMEM);
            return;
        }

        uint64_t flags = prot_to_flags(prot);

        for (uint64_t i = 0; i < pages; i++)
        {
            uint64_t v = addr + i * PAGE_SIZE_4K;
            uint64_t frame = pmm::alloc_frame();
            bool ok = frame != 0;

            if (ok)
            {
                memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
                ok = paging::map_user_page(v, frame, flags) &&
                     paging::set_user_page_flags(v, flags);   // drops USER for PROT_NONE
                if (!ok && !paging::page_frame(v))
                    pmm::free_frame(frame);
            }

            if (!ok)
            {
                release_range(addr, i + 1);
                regs->rax = SYSCALL_ERR(ENOMEM);
                return;
            }
        }

        current->mmap_cursor = addr + size;
        if (current->mmap_cursor >= USER_MMAP_LIMIT)
            current->mmap_cursor = USER_MMAP_BASE;

        regs->rax = addr;
    }

    void sys_munmap(user_regs* regs, iret_frame*)
    {
        uint64_t addr = regs->rdi;
        uint64_t len  = regs->rsi;
        regs->rax = SYSCALL_ERR(EINVAL);

        if (!len || (addr & (PAGE_SIZE_4K - 1)))
            return;

        uint64_t pages = (len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        if (!range_in(addr, pages * PAGE_SIZE_4K, USER_MMAP_BASE, USER_MMAP_LIMIT))
            return;

        release_range(addr, pages);
        regs->rax = 0;
    }

    void sys_mprotect(user_regs* regs, iret_frame*)
    {
        uint64_t addr = regs->rdi;
        uint64_t len  = regs->rsi;
        uint64_t prot = regs->rdx;
        regs->rax = SYSCALL_ERR(EINVAL);

        if (!len || (addr & (PAGE_SIZE_4K - 1)) ||
            (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)))
            return;

        uint64_t pages = (len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        uint64_t size  = pages * PAGE_SIZE_4K;

        // The image, heap, mmap region and stack are the app's to change.
        // program_info and argv are not.
        bool allowed =
            range_in(addr, size, USER_IMAGE_VADDR, USER_MMAP_LIMIT) ||
            range_in(addr, size, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP);
        if (!allowed)
            return;

        // All or nothing: every page must exist before any is changed.
        for (uint64_t i = 0; i < pages; i++)
            if (!paging::page_frame(addr + i * PAGE_SIZE_4K))
            {
                regs->rax = SYSCALL_ERR(ENOMEM);
                return;
            }

        uint64_t flags = prot_to_flags(prot);
        for (uint64_t i = 0; i < pages; i++)
            paging::set_user_page_flags(addr + i * PAGE_SIZE_4K, flags);

        regs->rax = 0;
    }
} // namespace process
