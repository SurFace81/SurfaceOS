// Processes: table, ELF program loading, the scheduler, and the process and
// memory syscalls. See process.h for the scheduling model.
//
// User memory is W^X throughout: code is read+execute, everything writable
// (data, heap, stack) is NX. mmap/mprotect are the one sanctioned way to get
// RWX memory, which a JIT or tcc -run needs.

#include "../../include/cpu/process.h"
#include "../../include/cpu/signal.h"
#include "../../include/cpu/percpu.h"
#include "../../include/cpu/spinlock.h"
#include "../../include/acpi/acpi.h"
#include "../../include/cpu/smp.h"
#include "../../include/cpu/task.h"
#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/sffile.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/sfos/status.h"
#include "../../sdk/include/sfos/process.h"
#include "../../sdk/include/sfos/admin.h"
#include "../../sdk/include/abi/sdkimage.h"
#include "../../include/cpu/wait.h"
#include "../../include/cpu/elf.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/irq.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/obj/object.h"
#include "../../include/obj/event.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/tty.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/term.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/rtc.h"
#include "../../include/drivers/uart.h"
#include "../../include/fs/devfs.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/time.h"
#include "../../sdk/include/abi/fcntl.h"

#define USER_CS             0x23
#define USER_SS             0x2B
#define RFLAGS_USER         0x202       // IF + reserved bit 1
#define INT80_LENGTH        2           // `int $0x80` is CD 80
#define TIME_SLICE_TICKS    10          // PIT ticks (~10 ms at 1 kHz), per unit of weight

// Syscall failure: -errno in rax (Linux convention, see abi/errno.h).
#define SYSCALL_ERR(e)      ((uint64_t)(sint64_t)-(e))

namespace process
{
    // -----------------------------------------------------------------------
    // Process table
    // -----------------------------------------------------------------------

    // A process: a running program - its address space, handles, roots,
    // signals and place in the process tree. What runs are its threads: the
    // one it starts with and those it creates (SfThread). The scheduler
    // picks threads.
    //
    // Process state: Live (its thread runs or sleeps), Stopped (job control:
    // its thread is not picked until SIGCONT), Zombie (exited, the parent
    // has not reaped it yet). Thread state: Runnable or Blocked (asleep on a
    // wait queue).
    enum class State  : uint8_t { Unused, Live, Stopped, Zombie };
    enum class TState : uint8_t { Unused, Runnable, Blocked };

    struct proc_obj;
    struct thread_obj;
    struct Process;

    struct Thread
    {
        TState      state;
        Process*    proc;           // null in an unused slot
        uint32_t    cpu;            // the CPU that runs it
        bool        running;        // on its CPU right now
        bool        doomed;         // its process ended while it ran on another
                                    // CPU: it ends at its next kernel entry

        // What a handle to the thread refers to (its exit status for
        // Join); the thread holds one reference until it ends. Null for
        // the thread a process starts with: nobody can join that one.
        thread_obj* obj;

        // The user stack of a created thread (the kernel's to free), in
        // pages from stack_base; 0 for the first thread, whose stack is the
        // process's.
        uint64_t    stack_base;
        uint64_t    stack_pages;

        // Blocked: the queue slept on (null once woken), the next sleeper
        // on it, whether wake_up ended the sleep, and the deadline tick (0
        // for none).
        wait_queue* wq;
        Thread*     wq_next;
        bool        woken;
        uint64_t    wake_tick;

        // The call running now. restart_pending: a signal ended a sleep
        // inside it before it had a result (syscall_interrupted). On the way
        // back to ring 3 the call is either restarted - RIP stepped back
        // over `int 0x80`, rax = syscall_nr - or fails with EINTR.
        uint64_t    syscall_nr;
        bool        restart_pending;

        // User state a fresh image or a fork child starts from. Once the
        // thread runs, its live user state is the trap frame on its own
        // kernel stack, not this.
        cpu_context ctx;
        uint8_t     fpu[512] __attribute__((aligned(16)));   // FXSAVE area
        // Kernel stack for traps taken while this thread runs (TSS RSP0),
        // and the task that runs on it. Owned by the table *slot*, not by
        // the thread: terminate() can free a thread while running on this
        // very stack, so it is released later, from another task
        // (reclaim_kernel_stacks).
        uint64_t    kstack;     // base (direct-map virtual), 0 when the slot has none
        Task        task;
    };

    struct Process
    {
        State       state;
        uint32_t    cpu;            // its home CPU: where its first thread ran;
                                    // it alone acts on the process's signals
        bool        reap_when_empty;    // free the slot once its last (doomed)
                                        // thread is gone
        pid_t       pid;
        pid_t       ppid;           // 0: started by the console, or an orphan
        pid_t       pgid;           // process group, for job control
        pid_t       wait_pid;       // wait4 sleeping: which child (<= 0: any)
        uint64_t    wait_opts;      // wait4 sleeping: WUNTRACED/WCONTINUED/...

        // wait4 sleeps here; notify_parent wakes it on every child event.
        wait_queue  child_wq;

        // A kernel process (the console): no user address space, never
        // signalled or killed, runs kernel code on its task only.
        bool        kernel;
        int         exit_status;    // valid in Zombie
        SfStatus    sf_status;      // what SfMain or the last thread returned;
                                    // SF_ABORTED when something else ended it

        // Job control: set when the process is stopped, cleared when the
        // parent reports it. cont_pending does the same for SIGCONT.
        int         stop_status;
        bool        stop_pending;   // a stop the parent has not seen yet
        bool        cont_pending;   // ditto for a resume

        // Signals.
        sig::signal_state sig;
        bool        mask_saved;     // sigsuspend: restore this on sigreturn
        sigset_t    saved_mask;

        char        name[32];

        // The screen it shows on and reads keys from, and who gave it that
        // screen's input when it owns it (input owners, below).
        uint32_t    screen;
        pid_t       input_giver;
        bool        console_raw;    // SF_CONSOLE_RAW (sfos/console.h)
        bool        admin;          // the admin right (sfos/admin.h)
        bool        from_console;   // started by its screen's console (the log says so)

        uint64_t    cr3;
        uint64_t    brk_start;      // end of the ELF image
        uint64_t    brk;            // current program break
        uint64_t    mmap_cursor;    // where the next mmap search starts

        // Kernel objects the process holds, by handle. A file descriptor is a
        // handle holding a File.
        handle_table handles;

        // The process as a kernel object (what a handle to it refers to).
        // The slot holds one reference until the process exits.
        proc_obj*   obj;

        // POSIX file state beyond the descriptors: cwd (referenced vnode)
        // and umask.
        vnode*      cwd;
        uint32_t    umask;

        // The SurfaceOS roots (sffile.h): data, tmp and argN, each a
        // referenced directory or file. Free slots have v == nullptr.
        struct Root
        {
            char   name[8];
            vnode* v;
        };
        Root        roots[MAX_ROOTS];

        // The command line: argc NUL-terminated strings back to back
        // (kmalloc'ed), for SfProcess GetArgs.
        char*       args;
        uint32_t    args_size;
        uint32_t    argc;

    };

    static Process  table[MAX_PROCESSES];
    static Thread   threads[MAX_THREADS];

    // -----------------------------------------------------------------------
    // Process objects
    // -----------------------------------------------------------------------
    //
    // What a handle to a process refers to. It is separate from the table
    // slot because it has to outlive the process: whoever holds a handle can
    // still read the exit status after the slot has been reused.

    struct proc_obj
    {
        kobject    hdr;         // type Process
        pid_t      pid;
        bool       exited;
        int        status;      // exit status once exited (wait format)
        SfStatus   sf_status;   // the same as an SfStatus (SfProcess Wait)
        wait_queue changed;     // woken on exit
        bool       used;        // pool slot taken
    };

    const uint32_t  MAX_PROC_OBJS = MAX_PROCESSES * 2;
    static proc_obj proc_objs[MAX_PROC_OBJS];

    static void proc_obj_destroy(kobject* o)
    {
        ((proc_obj*)o)->used = false;
    }

    // Waitable: signaled once the process has exited.
    static bool proc_obj_signaled(kobject* o)
    {
        return ((proc_obj*)o)->exited;
    }

    static wait_queue* proc_obj_waitq(kobject* o)
    {
        return &((proc_obj*)o)->changed;
    }

    static const kobject_ops proc_obj_ops =
    {
        obj_type::Process, "process", proc_obj_destroy,
        proc_obj_signaled, proc_obj_waitq, nullptr,
    };

    // A new object for process `pid`; the caller holds its reference.
    static proc_obj* proc_obj_new(pid_t pid)
    {
        for (uint32_t i = 0; i < MAX_PROC_OBJS; i++)
        {
            proc_obj* o = &proc_objs[i];
            if (o->used)
                continue;
            kobj::init(&o->hdr, &proc_obj_ops);
            o->pid         = pid;
            o->exited      = false;
            o->status      = 0;
            o->sf_status   = SF_ABORTED;
            o->changed.head = nullptr;
            o->used        = true;
            return o;
        }
        return nullptr;
    }

    // The process behind p->obj is gone: record how, wake the waiters and
    // drop the slot's reference.
    static void proc_obj_exit(Process* p, int status)
    {
        proc_obj* o = p->obj;
        if (!o)
            return;
        o->exited = true;
        o->status = status;
        o->sf_status = p->sf_status;
        wait::wake_up(&o->changed);
        p->obj = nullptr;
        kobj::put(&o->hdr);
    }
    // -----------------------------------------------------------------------
    // Thread objects
    // -----------------------------------------------------------------------
    //
    // What a handle to a thread refers to: it outlives the thread, so Join
    // can read the exit status after the thread has gone.

    struct thread_obj
    {
        kobject    hdr;         // type Thread
        bool       exited;
        SfStatus   status;      // the thread's SfStatus once exited
        wait_queue changed;     // woken on exit
    };

    static void thread_obj_destroy(kobject* o)
    {
        kfree(o);
    }

    // Waitable: signaled once the thread has ended.
    static bool thread_obj_signaled(kobject* o)
    {
        return ((thread_obj*)o)->exited;
    }

    static wait_queue* thread_obj_waitq(kobject* o)
    {
        return &((thread_obj*)o)->changed;
    }

    static const kobject_ops thread_obj_ops =
    {
        obj_type::Thread, "thread", thread_obj_destroy,
        thread_obj_signaled, thread_obj_waitq, nullptr,
    };

    // A new thread object; the caller holds its reference.
    static thread_obj* thread_obj_new()
    {
        thread_obj* o = (thread_obj*)kmalloc(sizeof(thread_obj));
        if (!o)
            return nullptr;
        kobj::init(&o->hdr, &thread_obj_ops);
        o->exited = false;
        o->status = SF_SUCCESS;
        o->changed.head = nullptr;
        return o;
    }

    // The thread running on this CPU and its process, the time slice it
    // has used, and where this CPU's round-robin search goes on: each
    // CPU's own (percpu.h), named here as if they were variables.
    static inline Cpu* this_cpu() { return cpu::current(); }
    #define cur_thread  (this_cpu()->thread)
    #define current     (this_cpu()->proc)
    #define slice_ticks (this_cpu()->slice_ticks)
    #define last_slot   (this_cpu()->last_slot)

    static pid_t    next_pid  = 1;

    // Ctrl+Alt+C (keyboard.cpp): the screen whose programs are to end, -1
    // for none. Acted on at the next scheduling decision - the keyboard IRQ
    // can land anywhere.
    static volatile sint32_t end_screen = -1;
    static volatile sint32_t pause_screen = -1;     // Ctrl+Alt+Z, the same way

    static inline bool screen_request()
    {
        return end_screen >= 0 || pause_screen >= 0;
    }

    // Each CPU's boot task becomes its idle task once the kernel is up: it
    // runs whenever nothing else can (this_cpu()->idle_task).

    const uint64_t KERNEL_STACK_FRAMES = KERNEL_STACK_SIZE / 4096;

    // Clean FPU/SSE state every new program starts from.
    static uint8_t fpu_template[512] __attribute__((aligned(16)));

    static inline uint64_t read_cr3()
    {
        uint64_t v;
        asm volatile("mov %%cr3, %0" : "=r"(v));
        return v;
    }

    static inline uint64_t kstack_top(const Thread* t)
    {
        return t->kstack + KERNEL_STACK_SIZE;
    }

    // Hand the kernel stacks of empty thread slots back. A thread that ends
    // frees its slot while still running on that stack, so a stack can only
    // go once some other task runs - the caller's own is never touched.
    static void reclaim_kernel_stacks()
    {
        for (uint32_t i = 0; i < MAX_THREADS; i++)
        {
            Thread* t = &threads[i];
            if (t->state != TState::Unused || !t->kstack || &t->task == this_cpu()->running_task)
                continue;
            pmm::free_frames(virt_to_phys((void*)t->kstack), KERNEL_STACK_FRAMES);
            t->kstack = 0;
        }
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
            if (*p == '/')
                name = p + 1;

        uint32_t i = 0;
        for (; name[i] && i < sizeof(Process::name) - 1; i++)
            dst[i] = name[i];
        dst[i] = '\0';
    }

    // A free thread slot with a kernel stack, cleared, for process p; its
    // state stays Unused until the process starts. nullptr: none, or no
    // memory for the stack.
    static Thread* alloc_thread(Process* p)
    {
        for (uint32_t i = 0; i < MAX_THREADS; i++)
        {
            Thread* t = &threads[i];
            if (t->state != TState::Unused || t->proc)
                continue;

            uint64_t ks = t->kstack;    // belongs to the slot; survives reuse
            memory::memset((uint8_t*)t, 0x00, sizeof(Thread));
            t->kstack = ks;
            if (!t->kstack)
            {
                uint64_t frames = pmm::alloc_frames(KERNEL_STACK_FRAMES);
                if (!frames)
                    return nullptr;
                t->kstack = (uint64_t)phys_to_virt(frames);
            }
            t->proc = p;
            t->cpu  = p->cpu;
            return t;
        }
        return nullptr;
    }

    static void unlink_wait(Thread* t);

    // The thread is gone: off any queue, its Join reports `status`, its
    // slot is free (the kernel stack stays with the slot). Its user stack
    // goes with the address space, or with end_created_thread.
    static void free_thread(Thread* t, SfStatus status)
    {
        unlink_wait(t);
        if (t->obj)
        {
            t->obj->exited = true;
            t->obj->status = status;
            wait::wake_up(&t->obj->changed);
            kobj::put(&t->obj->hdr);
            t->obj = nullptr;
        }
        if (t == cur_thread)
            cur_thread = nullptr;
        t->state   = TState::Unused;
        t->proc    = nullptr;
        t->running = false;
        t->doomed  = false;
    }

    // End every thread of p (the process is going). One running on another
    // CPU right now cannot be freed from here - that CPU is on its stack and
    // in its address space: it is doomed instead, and ends at its next
    // kernel entry (a tick at the latest). How many are left that way.
    static uint32_t end_threads(Process* p, SfStatus status)
    {
        uint32_t left = 0;
        for (uint32_t i = 0; i < MAX_THREADS; i++)
        {
            Thread* t = &threads[i];
            if (t->proc != p)
                continue;
            if (t->running && t != cur_thread)
            {
                t->doomed = true;
                left++;
            }
            else
                free_thread(t, status);
        }
        return left;
    }

    // The process's address space goes (no thread of it runs anywhere).
    static void release_address_space(Process* p)
    {
        if (!p->cr3)
            return;
        if (read_cr3() == p->cr3)
        {
            paging::switch_address_space(paging::kernel_pml4());
            this_cpu()->cr3 = paging::kernel_pml4();
        }
        paging::destroy_address_space(p->cr3);
        p->cr3 = 0;
    }

    static uint32_t count_threads(const Process* p)
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < MAX_THREADS; i++)
            if (threads[i].proc == p && threads[i].state != TState::Unused)
                n++;
        return n;
    }

    // The thread a new process starts with (alloc_process made it).
    static Thread* first_thread(Process* p)
    {
        for (uint32_t i = 0; i < MAX_THREADS; i++)
            if (threads[i].proc == p)
                return &threads[i];
        return nullptr;
    }

    // The CPU with the fewest threads: where a new process or thread goes.
    static uint32_t least_loaded_cpu()
    {
        uint32_t cpus = smp::running();
        uint32_t load[acpi::MAX_CPUS] = {};
        for (uint32_t i = 0; i < MAX_THREADS; i++)
            if (threads[i].state != TState::Unused && threads[i].cpu < cpus)
                load[threads[i].cpu]++;
        uint32_t best = 0;
        for (uint32_t c = 1; c < cpus; c++)
            if (load[c] < load[best])
                best = c;
        return best;
    }

    // A new process with its first thread, both slots taken but not
    // started yet (state Unused until start()).
    static Process* alloc_process()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            if (table[i].state != State::Unused)
                continue;

            Process* p = &table[i];
            memory::memset((uint8_t*)p, 0x00, sizeof(Process));
            p->cpu = least_loaded_cpu();
            Thread* t = alloc_thread(p);
            if (!t)
                return nullptr;         // slot stays Unused
            p->pid = next_pid++;
            if (next_pid <= 0)
                next_pid = 1;
            p->pgid = p->pid;           // its own group until setpgid says otherwise
            p->sf_status = SF_ABORTED;
            p->obj = proc_obj_new(p->pid);
            if (!p->obj)
            {
                free_thread(t, SF_ABORTED);
                return nullptr;         // slot stays Unused
            }
            sig::init(&p->sig);

            handles::init(&p->handles);
            p->umask = 022;
            p->cwd = vfs::cwd_ref();        // inherit the system cwd
            memory::memset((uint8_t*)p->roots, 0x00, sizeof(p->roots));
            p->args      = nullptr;
            p->args_size = 0;
            p->argc      = 0;
            return p;
        }
        return nullptr;
    }

    // Give p root `name`, taking over one reference to v. When all slots
    // are taken the reference is dropped.
    static void add_root(Process* p, const char* name, vnode* v)
    {
        for (uint32_t i = 0; i < MAX_ROOTS; i++)
        {
            if (p->roots[i].v)
                continue;
            copy_bytes((uint8_t*)p->roots[i].name, (const uint8_t*)name,
                       (uint64_t)strlen(name) + 1);
            p->roots[i].v = v;
            return;
        }
        vfs::unref(v);
    }

    // Release the roots and the command line.
    static void drop_roots(Process* p)
    {
        for (uint32_t i = 0; i < MAX_ROOTS; i++)
        {
            if (p->roots[i].v)
                vfs::unref(p->roots[i].v);
            p->roots[i].v = nullptr;
        }
        if (p->args)
            kfree(p->args);
        p->args      = nullptr;
        p->args_size = 0;
        p->argc      = 0;
    }

    static void free_process(Process* p)
    {
        if (p->obj)
        {
            // Never ran to an exit (a launch that failed half-way).
            p->obj->exited = true;
            wait::wake_up(&p->obj->changed);
            kobj::put(&p->obj->hdr);
            p->obj = nullptr;
        }
        handles::close_all(&p->handles);
        if (p->cwd)
        {
            vfs::unref(p->cwd);
            p->cwd = nullptr;
        }
        drop_roots(p);
        if (end_threads(p, SF_ABORTED))
        {
            // A thread still runs on another CPU: the slot goes with it.
            p->state = State::Zombie;
            p->reap_when_empty = true;
            return;
        }
        release_address_space(p);
        p->state = State::Unused;
        p->pid = 0;
    }

    // Start a process alloc_process made: its first thread becomes
    // runnable.
    static void start(Process* p)
    {
        p->state = State::Live;
        first_thread(p)->state = TState::Runnable;
    }

    // A process that still exists and can be signalled. A stopped process
    // counts: SIGCONT is the whole point of it being there.
    static inline bool alive(const Process* p)
    {
        return p->state == State::Live || p->state == State::Stopped;
    }

    static Process* find_live(pid_t pid)
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->pid == pid && alive(p))
                return p;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Input owners
    // -----------------------------------------------------------------------

    // Each screen gives its keys to one process, its input owner (-1: none,
    // and the keys are dropped). The console owns screen 1 and gives it to
    // the program it starts; a program that owns it gives it on with
    // Start(SF_START_GIVE_INPUT). When the owner ends, the screen goes back
    // to whoever gave it, or to the console if that one is gone too.
    static pid_t      input_owner[TERM_ALL_SCREENS];
    static wait_queue input_owner_wq;   // ReadLine waits here for its turn

    // Each shown screen has a console, CMD.BIN (-1 while there is none, -2
    // when it cannot be started): who the input goes back to in the end.
    static pid_t screen_console[TERM_ALL_SCREENS];
    static wait_queue keeper_wq;        // cmdkeeper: woken when one is gone

    // The title bar names the input owner of a shown screen.
    static void set_input_owner(uint32_t screen, pid_t pid)
    {
        input_owner[screen] = pid;
        wait::wake_up(&input_owner_wq);
        if (screen >= TERM_SCREENS || pid < 0)
            return;
        Process* p = find_live(pid);
        uint32_t prev = term::selected();
        term::select(screen);
        term::set_program(p ? p->name : "");
        term::select(prev);
    }

    // p ends: whatever screen it owns goes back - to whoever gave it, if
    // that one still runs on the screen, or else to the screen's console.
    static void return_input(Process* p)
    {
        for (uint32_t s = 0; s < TERM_ALL_SCREENS; s++)
        {
            if (input_owner[s] != p->pid)
                continue;
            Process* giver = find_live(p->input_giver);
            if (giver && giver != p && giver->screen == s)
                set_input_owner(s, giver->pid);
            else
                set_input_owner(s, screen_console[s] >= 0 && screen_console[s] != p->pid
                                   ? screen_console[s] : -1);
        }
    }

    static bool owns_input(void*)
    {
        return current && input_owner[current->screen] == current->pid;
    }

    void end_screen_programs(uint32_t screen)
    {
        end_screen = (sint32_t)screen;
    }

    void pause_screen_programs(uint32_t screen)
    {
        pause_screen = (sint32_t)screen;
    }

    bool current_admin()
    {
        return current && current->admin;
    }

    uint64_t list_programs(SfProcessInfo* out, uint64_t max)
    {
        uint64_t n = 0;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (!alive(p) || p->kernel)
                continue;
            if (n < max)
            {
                SfProcessInfo* e = &out[n];
                memory::memset((uint8_t*)e, 0, sizeof(*e));
                e->Id     = (uint64_t)p->pid;
                e->Screen = p->screen < TERM_SCREENS ? p->screen + 1 : 0;
                e->Paused = p->state == State::Stopped ? 1 : 0;
                copy_bytes((uint8_t*)e->Name, (const uint8_t*)p->name, sizeof(e->Name) - 1);
            }
            n++;
        }
        return n;
    }

    static void post_signal(Process* p, int n);

    bool end_program(pid_t pid)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel)
            return false;
        post_signal(p, SIGKILL);
        return true;
    }

    uint32_t current_screen()
    {
        return current ? current->screen : 0;
    }

    bool console_raw()
    {
        return current && current->console_raw;
    }

    void set_console_raw(bool raw)
    {
        if (current)
            current->console_raw = raw;
    }

    pid_t screen_input_owner(uint32_t screen)
    {
        return screen < TERM_ALL_SCREENS ? input_owner[screen] : -1;
    }

    bool wait_for_input()
    {
        return wait::wait_event(&input_owner_wq, owns_input, nullptr, 0);
    }

    // -----------------------------------------------------------------------
    // Background programs
    // -----------------------------------------------------------------------

    // A program started with `&` runs on a hidden screen of its own (the
    // programs it starts share it), and what it prints in SF_CONSOLE_LINE
    // also goes to a log in its data folder. The screen and the log go when
    // the last program on the screen ends.
    struct ScreenLog
    {
        vnode*   v;
        uint64_t off;
    };
    static ScreenLog screen_log[TERM_ALL_SCREENS];

    void log_output(const char* s, uint64_t len)
    {
        if (!current || current->console_raw)
            return;
        ScreenLog* l = &screen_log[current->screen];
        uint64_t done = 0;
        if (l->v && len && l->v->ops->write(l->v, l->off, s, len, &done) == 0)
            l->off += done;
    }

    // p ends: the hidden screen it ran on goes once nothing else runs there.
    static void release_screen(Process* p)
    {
        uint32_t s = p->screen;
        if (s < TERM_SCREENS)
            return;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q != p && alive(q) && !q->kernel && q->screen == s)
                return;
        }
        ScreenLog* l = &screen_log[s];
        if (l->v)
        {
            if (l->v->ops->fsync)
                l->v->ops->fsync(l->v);
            vfs::unref(l->v);
        }
        l->v   = nullptr;
        l->off = 0;
        term::close_hidden(s);
    }

    static char* put_num(char* p, uint32_t v, uint32_t digits)
    {
        for (uint32_t i = digits; i-- > 0; v /= 10)
            p[i] = (char)('0' + v % 10);
        return p + digits;
    }

    // A new log in p's data folder, console_YYYY-MM-DD_hh-mm-ss.log - with
    // _2, _3... when that name is taken. Its name goes to name (64 bytes).
    static vnode* create_log(Process* p, char* name)
    {
        vnode* dir = nullptr;
        for (uint32_t i = 0; i < MAX_ROOTS && !dir; i++)
            if (p->roots[i].v && strcmp(p->roots[i].name, "data") == 0)
                dir = p->roots[i].v;
        if (!dir || !dir->ops->create)
            return nullptr;

        rtc_time t;
        rtc::read(&t);
        char base[40];
        char* q = base;
        for (const char* c = "console_"; *c; c++)
            *q++ = *c;
        q = put_num(q, t.year, 4);    *q++ = '-';
        q = put_num(q, t.month, 2);   *q++ = '-';
        q = put_num(q, t.day, 2);     *q++ = '_';
        q = put_num(q, t.hours, 2);   *q++ = '-';
        q = put_num(q, t.minutes, 2); *q++ = '-';
        q = put_num(q, t.seconds, 2);
        uint32_t base_len = (uint32_t)(q - base);

        for (uint32_t n = 1; n < 100; n++)
        {
            copy_bytes((uint8_t*)name, (const uint8_t*)base, base_len);
            char* e = name + base_len;
            if (n > 1)
            {
                *e++ = '_';
                e = put_num(e, n, n < 10 ? 1 : 2);
            }
            copy_bytes((uint8_t*)e, (const uint8_t*)".log", 5);

            vnode* v = nullptr;
            sint64_t rc = dir->ops->lookup(dir, name, &v);
            if (rc == 0)
            {
                vfs::unref(v);          // taken: the next suffix
                continue;
            }
            if (rc == -ENOENT && dir->ops->create(dir, name, 0644, &v) == 0)
                return v;
            return nullptr;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Contexts
    // -----------------------------------------------------------------------

    static void fpu_save(uint8_t* area)    { asm volatile("fxsave (%0)"  :: "r"(area) : "memory"); }
    static void fpu_restore(uint8_t* area) { asm volatile("fxrstor (%0)" :: "r"(area) : "memory"); }

    // Leave the running thread (if there still is one): its FPU state is
    // the live one until now.
    static void leave_current()
    {
        if (cur_thread)
            fpu_save(cur_thread->fpu);
    }

    // Run thread `t`: its process's address space, its FPU state and kernel
    // stack become live and its task resumes - inside its own trap handler,
    // or at its first entry to ring 3. Returns when the calling task is
    // switched back to.
    static void switch_thread(Thread* t)
    {
        bkl::check_switch();
        leave_current();
        if (cur_thread)
            cur_thread->running = false;
        cur_thread = t;
        current = t->proc;
        t->running = true;
        cpu::set_kernel_stack(kstack_top(t));
        paging::switch_address_space(t->proc->cr3);
        this_cpu()->cr3 = t->proc->cr3;
        fpu_restore(t->fpu);
        task::switch_to(&t->task);
    }

    // Run a kernel task (idle): no thread is current meanwhile.
    static void switch_kernel_task(Task* t)
    {
        bkl::check_switch();
        leave_current();
        if (cur_thread)
            cur_thread->running = false;
        cur_thread = nullptr;
        current = nullptr;
        paging::switch_address_space(paging::kernel_pml4());
        this_cpu()->cr3 = paging::kernel_pml4();
        task::switch_to(t);
    }

    static void initial_context(cpu_context* ctx, uint64_t entry, uint64_t rsp)
    {
        memory::memset((uint8_t*)ctx, 0x00, sizeof(cpu_context));
        ctx->iret.rip    = entry;
        ctx->iret.cs     = USER_CS;
        ctx->iret.rflags = RFLAGS_USER;
        ctx->iret.rsp    = rsp;         // 16-byte aligned
        ctx->iret.ss     = USER_SS;
    }

    // -----------------------------------------------------------------------
    // Program loading
    // -----------------------------------------------------------------------

    // argv/envp collection for execve and console launches. Strings live in
    // one growable kernel buffer, per-string offsets in two growable arrays.
    // The combined size of strings + pointers is capped at ARG_MAX, like
    // Linux; overflow is -E2BIG.
    struct ArgEnv
    {
        char*     data;
        uint32_t  data_used;
        uint32_t  data_cap;

        uint32_t* a_off;        // argv string offsets into data
        uint32_t  a_count;
        uint32_t  a_cap;

        uint32_t* e_off;        // envp string offsets into data
        uint32_t  e_count;
        uint32_t  e_cap;

        bool init()
        {
            data = nullptr; a_off = nullptr; e_off = nullptr;
            data_used = data_cap = a_count = a_cap = e_count = e_cap = 0;

            data_cap = 1024;  a_cap = 16;  e_cap = 16;
            data  = (char*)kmalloc(data_cap);
            a_off = (uint32_t*)kmalloc(a_cap * sizeof(uint32_t));
            e_off = (uint32_t*)kmalloc(e_cap * sizeof(uint32_t));
            return data && a_off && e_off;
        }

        void destroy()
        {
            if (data)  kfree(data);
            if (a_off) kfree(a_off);
            if (e_off) kfree(e_off);
            data = nullptr; a_off = nullptr; e_off = nullptr;
        }

        // Double the string buffer, never past ARG_MAX. 0 or -errno.
        int grow_data()
        {
            if (data_cap >= ARG_MAX)
                return -E2BIG;
            uint32_t nc = data_cap * 2;
            if (nc > ARG_MAX)
                nc = ARG_MAX;
            char* nd = (char*)kmalloc(nc);
            if (!nd)
                return -ENOMEM;
            copy_bytes((uint8_t*)nd, (const uint8_t*)data, data_used);
            kfree(data);
            data = nd;
            data_cap = nc;
            return 0;
        }

        // A string of `len` bytes is already sitting at data+data_used
        // (NUL included in len+1). Record it. 0 or -errno.
        int finish(bool env, uint32_t len)
        {
            // ARG_MAX counts the strings and their stack pointers.
            if ((uint64_t)data_used + len + 1 +
                ((uint64_t)a_count + e_count + 1) * 8 > ARG_MAX)
                return -E2BIG;

            if (env)
            {
                if (e_count == e_cap)
                {
                    uint32_t nc = e_cap * 2;
                    uint32_t* na = (uint32_t*)kmalloc(nc * sizeof(uint32_t));
                    if (!na)
                        return -ENOMEM;
                    copy_bytes((uint8_t*)na, (const uint8_t*)e_off,
                               e_count * sizeof(uint32_t));
                    kfree(e_off);
                    e_off = na;
                    e_cap = nc;
                }
                e_off[e_count++] = data_used;
            }
            else
            {
                if (a_count == a_cap)
                {
                    uint32_t nc = a_cap * 2;
                    uint32_t* na = (uint32_t*)kmalloc(nc * sizeof(uint32_t));
                    if (!na)
                        return -ENOMEM;
                    copy_bytes((uint8_t*)na, (const uint8_t*)a_off,
                               a_count * sizeof(uint32_t));
                    kfree(a_off);
                    a_off = na;
                    a_cap = nc;
                }
                a_off[a_count++] = data_used;
            }

            data[data_used + len] = '\0';
            data_used += len + 1;
            return 0;
        }

        // Kernel string (console launches, execve fallbacks).
        int push_kstr(bool env, const char* s)
        {
            uint32_t len = 0;
            while (s[len])
                len++;

            while (data_used + len + 1 > data_cap)
            {
                int g = grow_data();
                if (g)
                    return g;
            }
            copy_bytes((uint8_t*)data + data_used, (const uint8_t*)s, len);
            return finish(env, len);
        }

        // User string: read straight into the buffer, growing on truncation.
        int push_user(bool env, uint64_t user_str)
        {
            for (;;)
            {
                if (data_used == data_cap)
                {
                    int g = grow_data();
                    if (g)
                        return g;
                }

                uint32_t room = data_cap - data_used;
                sint64_t r = uaccess::strncpy_from_user(data + data_used,
                                                        user_str, room);
                if (r == -1)
                    return -EFAULT;
                if (r == -2)
                {
                    if (data_cap >= ARG_MAX)
                        return -E2BIG;
                    int g = grow_data();
                    if (g)
                        return g;
                    continue;
                }
                return finish(env, (uint32_t)r);
            }
        }
    };

    struct Image
    {
        uint64_t cr3;
        uint64_t entry;
        uint64_t image_end;
        uint64_t rsp;           // initial stack pointer

        // Where the program starts: in the SDK runtime.
        uint64_t sdk_start;

        // Its command line (see Process::args), kmalloc'ed.
        char*    args;
        uint32_t args_size;
        uint32_t argc;
    };

    // Map [vaddr, vaddr+size) with zeroed 4 KiB user pages (active space).
    static bool map_user_region(uint64_t vaddr, uint64_t size, uint64_t flags)
    {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE_4K)
        {
            uint64_t frame = pmm::alloc_frame();
            if (!frame)
                return false;

            memory::memset((uint8_t*)phys_to_virt(frame), 0x00, PAGE_SIZE_4K);
            if (!paging::map_user_page(vaddr + off, frame, flags))
            {
                pmm::free_frame(frame);
                return false;
            }
        }
        return true;
    }

    // Write kernel data into a user page of the active address space through
    // the direct map (the pages may be read-only to the app).
    static void poke_user(uint64_t vaddr, const uint8_t* src, uint64_t len)
    {
        while (len)
        {
            uint64_t phys  = paging::page_frame(vaddr & ~(PAGE_SIZE_4K - 1));
            uint64_t off   = vaddr & (PAGE_SIZE_4K - 1);
            uint64_t chunk = PAGE_SIZE_4K - off;
            if (chunk > len)
                chunk = len;

            copy_bytes((uint8_t*)phys_to_virt(phys + off), src, chunk);
            vaddr += chunk;
            src   += chunk;
            len   -= chunk;
        }
    }

    // Open the executable at `path` and validate it is a regular file of a
    // loadable size. Returns a referenced vnode (the caller unrefs it) or
    // nullptr with *out_err = errno. The ELF segments are read directly from
    // the vnode in load_program, so the file is never buffered whole.
    static vnode* open_executable(const char* path, vnode* cwd,
                                  uint64_t* out_size, int* out_err)
    {
        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, cwd, &v, false);
        if (rc != 0)
        {
            *out_err = (int)-rc;
            return nullptr;
        }

        if (v->type != vtype::REG)
        {
            vfs::unref(v);
            *out_err = (v->type == vtype::DIR) ? EISDIR : EACCES;
            return nullptr;
        }

        if (v->size == 0)
        {
            vfs::unref(v);
            *out_err = ENOEXEC;
            return nullptr;
        }
        if (v->size > USER_IMAGE_MAX)
        {
            uart::printf("process: %s is too large (%u bytes)\n",
                         path, (uint32_t)v->size);
            vfs::unref(v);
            *out_err = EFBIG;
            return nullptr;
        }

        *out_err = 0;
        *out_size = v->size;
        return v;
    }

    // Build a complete address space for `path` (resolved against `cwd`):
    // ELF segments, stack with the SysV argv/envp/auxv block. The active
    // address space is unchanged on return. Returns 0 or -errno.
    static sint64_t load_program(const char* path, vnode* cwd,
                                 const ArgEnv* ae, Image* out, uint64_t sdk_flags = 0)
    {
        int err = 0;
        uint64_t size = 0;
        vnode* v = open_executable(path, cwd, &size, &err);
        if (!v)
            return -(sint64_t)err;

        uint64_t as = paging::create_address_space();
        if (!as)
        {
            vfs::unref(v);
            return -ENOMEM;
        }

        uint64_t prev = read_cr3();
        paging::switch_address_space(as);

        elf::LoadResult lr = {0, 0, 0, 0, 0, false};
        bool ok = map_user_region(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                                  PAGE_WRITE | PAGE_NX);
        sint64_t elf_rc = -ENOEXEC;
        if (!ok)
            err = ENOMEM;
        else
        {
            lr = elf::load_file(v, size, &elf_rc);
            ok = lr.valid && lr.image_end <= USER_MMAP_BASE;
            if (!ok)
            {
                err = (elf_rc < 0) ? (int)-elf_rc : ENOEXEC;
                if (err == ENOEXEC)
                    uart::printf("process: %s is not a loadable ELF64 executable\n",
                                 path);
            }
        }

        // Every program starts in the SDK runtime with an empty stack, as
        // right after a call. (Its arguments are not passed on yet: `ae`
        // reaches the program with SfApp's argument fields.)
        uint64_t rsp = USER_STACK_TOP - 8;
        uint64_t sdk = 0;

        // The command line, back to back: for the SDK's start info and
        // for SfProcess GetArgs.
        uint32_t args_size = 0;
        for (uint32_t i = 0; i < ae->a_count; i++)
            args_size += (uint32_t)strlen(ae->data + ae->a_off[i]) + 1;
        char* args = (char*)kmalloc(args_size ? args_size : 1);
        if (!args)
        {
            err = ENOMEM;
            ok = false;
        }
        else
        {
            uint32_t off = 0;
            for (uint32_t i = 0; i < ae->a_count; i++)
            {
                const char* a = ae->data + ae->a_off[i];
                uint32_t len = (uint32_t)strlen(a) + 1;
                copy_bytes((uint8_t*)args + off, (const uint8_t*)a, len);
                off += len;
            }
        }

        if (ok)
        {
            char name[sizeof(Process::name)];
            copy_name(name, path);
            if (!sdkpage::install(name, args, args_size, ae->a_count, sdk_flags, &sdk))
            {
                err = ENOMEM;
                ok = false;
            }
        }

        paging::switch_address_space(prev);
        vfs::unref(v);

        if (!ok)
        {
            if (args)
                kfree(args);
            paging::destroy_address_space(as);
            return -(sint64_t)err;
        }

        out->cr3       = as;
        out->entry     = lr.entry;
        out->image_end = lr.image_end;
        out->rsp       = rsp;
        out->sdk_start = sdk;
        out->args      = args;
        out->args_size = args_size;
        out->argc      = ae->a_count;
        return 0;
    }

    static void adopt_image(Process* p, Thread* t, const Image* img, const char* path)
    {
        p->cr3         = img->cr3;
        p->brk_start   = img->image_end;
        p->brk         = img->image_end;
        p->mmap_cursor = USER_MMAP_BASE;
        copy_name(p->name, path);
        if (p->args)
            kfree(p->args);
        p->args      = img->args;
        p->args_size = img->args_size;
        p->argc      = img->argc;
        // SdkStart(SfMain): see abi/sdkimage.h.
        initial_context(&t->ctx, img->sdk_start, img->rsp);
        t->ctx.regs.rdi = img->entry;
        copy_bytes(t->fpu, fpu_template, sizeof(t->fpu));
    }

    // -----------------------------------------------------------------------
    // Scheduler
    // -----------------------------------------------------------------------

    // Post SIGCHLD to p's parent (defined with the rest of the signal
    // machinery, below).
    static void notify_parent(Process* p);

    // Wait queues are touched from IRQs (wake_up from the timer, the
    // keyboard), so every list change runs with interrupts off.
    static inline uint64_t irq_save()
    {
        uint64_t f;
        asm volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        return f;
    }

    static inline void irq_restore(uint64_t f)
    {
        asm volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
    }

    // Take t off the queue it sleeps on, if any.
    static void unlink_wait(Thread* t)
    {
        uint64_t f = irq_save();
        if (t->wq)
        {
            Thread** link = &t->wq->head;
            while (*link && *link != t)
                link = &(*link)->wq_next;
            if (*link)
                *link = t->wq_next;
            t->wq = nullptr;
            t->wq_next = nullptr;
        }
        irq_restore(f);
    }

    // Terminate `p`: release its memory, orphan its children (ppid 0) and
    // leave a zombie for its parent - the console's program leaves one for
    // the console, an orphan nothing.
    static void terminate(Process* p, int status)
    {
        uart::printf("process: pid %u %s ended, status %u\n", (uint32_t)p->pid, p->name,
                     (uint32_t)status);
        if (p->from_console)
            uart::printf("console: program end, status %u\n", (uint32_t)status);
        // Its threads end first: nothing of the process runs after this -
        // or, for one running on another CPU, after that CPU's next kernel
        // entry. The address space goes with the last of them.
        uint32_t left = end_threads(p, SF_ABORTED);
        if (!left)
            release_address_space(p);

        // POSIX: descriptors close and the cwd is released when the process
        // exits, not when the parent reaps the zombie. Other handles go with
        // them, and so do the roots.
        handles::close_all(&p->handles);
        if (p->cwd)
        {
            vfs::unref(p->cwd);
            p->cwd = nullptr;
        }
        drop_roots(p);

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

        // A screen's console ended: cmdkeeper starts a new one. The programs
        // it started run on.
        if (p->screen < TERM_SCREENS && screen_console[p->screen] == p->pid)
        {
            screen_console[p->screen] = -1;
            wait::wake_up(&keeper_wq);
        }
        return_input(p);
        release_screen(p);

        // Handles to the process see the exit now, whether or not a parent
        // reaps a zombie later.
        proc_obj_exit(p, status);

        // An orphan has nobody to report to (the console waits on a handle).
        // free_process keeps the slot while a doomed thread still runs.
        if (p->ppid == 0)
        {
            free_process(p);
        }
        else
        {
            p->state = State::Zombie;
            p->exit_status = status;
            notify_parent(p);
        }

        if (p == current)
            current = nullptr;
    }

    static bool child_event(Process* p);

    // wait_event condition for wait4: an event for the calling process.
    static bool current_child_event(void*)
    {
        return child_event(current);
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
            // A stop or a resume the parent asked to hear about is an event
            // in its own right, even though the child is still there.
            if ((q->stop_pending && (p->wait_opts & WUNTRACED)) ||
                (q->cont_pending && (p->wait_opts & WCONTINUED)))
                return true;
            has_child = true;
        }
        return !has_child;     // nothing left to wait for: let waitpid fail
    }

    static bool wake_ready(Thread* t)
    {
        // A deliverable signal ends any wait: this is what makes a blocking
        // read interruptible, and what lets a handler run at all while the
        // process sits in read() or wait().
        return sig::next_deliverable(&t->proc->sig) || t->woken ||
               (t->wake_tick && pit::ticks() >= t->wake_tick);
    }

    // The next thread to run on this CPU, round robin over its threads in
    // the thread table; a thread of a stopped process is passed over.
    // nullptr: none can run.
    static Thread* pick_next()
    {
        for (uint32_t i = 1; i <= MAX_THREADS; i++)
        {
            uint32_t slot = (last_slot + i) % MAX_THREADS;
            Thread* t = &threads[slot];
            if (t->state == TState::Unused || t->cpu != this_cpu()->index ||
                t->proc->state != State::Live)
                continue;

            if (t->state == TState::Blocked && wake_ready(t))
                t->state = TState::Runnable;

            if (t->state == TState::Runnable)
            {
                last_slot = slot;
                return t;
            }
        }
        return nullptr;
    }

    // Ctrl+Alt+C: every program on the screen ends, whatever it is doing -
    // SIGKILL, which nothing can catch, acted on by each process's own CPU
    // (a process running on another CPU cannot be torn down from here).
    static void post_signal(Process* p, int n);

    // A program on `screen` other than its console?
    static bool others_on(uint32_t screen)
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen && p->pid != screen_console[screen])
                return true;
        }
        return false;
    }

    // The screen's console is left alone while anything else runs there;
    // on its own, it ends too (and cmdkeeper starts a new one).
    static void end_programs_on(uint32_t screen)
    {
        bool others = others_on(screen);
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen &&
                (!others || p->pid != screen_console[screen]))
                post_signal(p, SIGKILL);
        }
    }

    // Ctrl+Alt+Z: the programs on the screen but its console pause - every
    // thread of each, SIGSTOP, which nothing can catch either - or, when
    // they are paused, go on (SIGCONT). The title bar says so.
    static void pause_programs_on(uint32_t screen)
    {
        bool any = false, paused = false;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen && p->pid != screen_console[screen])
            {
                any = true;
                paused |= p->state == State::Stopped || (p->sig.pending & SIGMASK(SIGSTOP));
            }
        }
        if (!any)
            return;

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen && p->pid != screen_console[screen])
                post_signal(p, paused ? SIGCONT : SIGSTOP);
        }
        term::set_paused(screen, !paused);
    }

    // -----------------------------------------------------------------------
    // Signals
    // -----------------------------------------------------------------------
    //
    // Delivery happens at the boundary back to ring 3, which is the only
    // place a user stack and a trap frame are both to hand. Two halves:
    //
    //   service_signals()  applies everything that needs no user code -
    //                      termination, stop, continue, discard - to every
    //                      process, including ones that are not running;
    //   deliver_signals()  builds a handler frame for the process that is
    //                      about to be resumed.
    //
    // Returning from a handler does not resume a kernel call: rt_sigreturn
    // restores the saved context wholesale. A syscall the signal interrupted
    // has already given up its sleep by then (EINTR or a restart, see
    // restart_syscall).

    const sigset_t STOP_SIGNALS = SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) |
                                  SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU);

    static inline int stop_code(int n) { return 0x7F | ((n & 0xFF) << 8); }

    static void notify_parent(Process* p)
    {
        if (p->ppid == 0)
            return;                     // the console waits on a handle
        Process* parent = find_live(p->ppid);
        if (parent)
        {
            sig::post(&parent->sig, SIGCHLD);
            wait::wake_up(&parent->child_wq);
        }
    }

    static void post_signal(Process* p, int n)
    {
        if (!sig::valid(n) || !alive(p) || p->kernel)
            return;

        // SIGCONT resumes before any question of handlers: a stopped
        // process cannot run its own handler until it is running again.
        if (n == SIGCONT)
        {
            p->sig.pending &= ~STOP_SIGNALS;
            if (p->state == State::Stopped)
            {
                p->state = State::Live;
                p->cont_pending = true;
                notify_parent(p);
            }
        }
        else if (sig::default_action(n) == sig::Action::Stop)
        {
            p->sig.pending &= ~SIGMASK(SIGCONT);
        }

        if (sig::discarded(&p->sig, n))
            return;                     // never pends: nothing would happen

        sig::post(&p->sig, n);
    }

    // Returns how many processes were signalled (0 means ESRCH).
    static int signal_group(pid_t pgid, int n)
    {
        int count = 0;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (!alive(p) || p->kernel || p->pgid != pgid)
                continue;
            count++;
            if (n)
                post_signal(p, n);
        }
        return count;
    }

    static void stop_process(Process* p, int n)
    {
        p->state = State::Stopped;
        p->stop_status  = stop_code(n);
        p->stop_pending = true;
        notify_parent(p);
    }

    // Apply every pending signal whose action needs no user code. Returns
    // true when `current` can no longer continue and the caller has to
    // reschedule.
    static bool service_signals()
    {
        bool switch_away = false;

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->cpu != this_cpu()->index)
                continue;               // its own CPU acts on it (it may be running there)

            while (alive(p) && p->sig.pending)
            {
                int n = sig::next_deliverable(&p->sig);
                if (!n)
                    break;

                // A stopped process acts only on what can kill it; the rest
                // waits for SIGCONT, which post_signal already handled.
                if (p->state == State::Stopped && n != SIGKILL)
                    break;

                if (sig::caught(&p->sig, n))
                    break;              // needs a user stack: see deliver_signals

                sig::clear(&p->sig, n);

                sig::Action a = (n == SIGKILL) ? sig::Action::Term
                                               : sig::default_action(n);
                switch (a)
                {
                    case sig::Action::Ign:
                    case sig::Action::Cont:
                        break;          // handled when it was posted

                    case sig::Action::Stop:
                        stop_process(p, n);
                        switch_away |= (p == current);
                        break;

                    case sig::Action::Term:
                    case sig::Action::Core:
                        switch_away |= (p == current);
                        terminate(p, signal_status(n));
                        break;
                }
            }
        }
        return switch_away;
    }

    // Make the return to ring 3 re-execute the syscall p was in: step back
    // over `int 0x80` with the syscall number in rax again (every other
    // argument register is still as the caller left it).
    static void restart_syscall(Thread* t, user_regs* regs, iret_frame* iret)
    {
        iret->rip -= INT80_LENGTH;
        regs->rax = t->syscall_nr;
    }

    // Build the handler frame on the user stack and point the trap frame at
    // the handler. False when the stack is unusable.
    static bool push_signal_frame(Process* p, int n, user_regs* regs,
                                  iret_frame* iret)
    {
        const k_sigaction act = p->sig.act[n];
        if (!act.restorer)
            return false;               // the SDK always supplies one

        // An interrupted syscall: with SA_RESTART the context saved in the
        // frame re-executes it after the handler returns; without, it fails
        // with EINTR (already in rax).
        if (cur_thread->restart_pending)
        {
            if (act.flags & SA_RESTART)
                restart_syscall(cur_thread, regs, iret);
            cur_thread->restart_pending = false;
        }

        cpu_context ctx;
        ctx.regs = *regs;
        ctx.iret = *iret;

        // sigsuspend installed a temporary mask; the frame carries the one
        // to go back to, so rt_sigreturn restores it without a second call.
        sigset_t old = p->mask_saved ? p->saved_mask : p->sig.blocked;
        p->mask_saved = false;

        uint64_t addr = sig::frame_addr(iret->rsp);
        sig::frame f;
        sig::build_frame(&f, &ctx, old, act.restorer);

        if (!uaccess::copy_to_user(addr, &f, sizeof(f)))
            return false;

        p->sig.blocked |= act.mask;
        if (!(act.flags & SA_NODEFER))
            p->sig.blocked |= SIGMASK(n);
        p->sig.blocked &= ~SIG_UNCATCHABLE;

        if (act.flags & SA_RESETHAND)
        {
            p->sig.act[n].handler = SIG_DFL;
            p->sig.act[n].flags &= ~(uint64_t)SA_RESETHAND;
        }

        // Enter the handler as if called: rdi is the signal number and the
        // frame's first word is the return address its `ret` will pop.
        // Everything else starts at zero rather than carrying the
        // interrupted values into a function that never declared them.
        memory::memset((uint8_t*)regs, 0x00, sizeof(user_regs));
        regs->rdi    = (uint64_t)n;
        iret->rip    = act.handler;
        iret->rsp    = addr;
        iret->cs     = USER_CS;
        iret->ss     = USER_SS;
        iret->rflags = RFLAGS_USER;     // DF clear, as the ABI requires
        return true;
    }

    // Deliver one caught signal to a process that is about to resume.
    // Returns true when the process died instead and the caller must
    // reschedule.
    static bool deliver_signals(Process* p, user_regs* regs, iret_frame* iret)
    {
        int n = sig::next_deliverable(&p->sig);
        if (!n || !sig::caught(&p->sig, n))
            return false;

        sig::clear(&p->sig, n);

        if (push_signal_frame(p, n, regs, iret))
            return false;

        // No usable stack to run the handler on. POSIX kills the process
        // with SIGSEGV, and it must not be catchable here or delivery would
        // recurse on the same broken stack.
        screen::printf("\n[pid %u %s killed: no room for a signal frame]\n",
                       (uint32_t)p->pid, p->name);
        uart::printf("process: pid %u signal frame unwritable\n", (uint32_t)p->pid);
        terminate(p, signal_status(SIGSEGV));
        return true;
    }

    // The system-wide part of every scheduling decision: act on Ctrl+Alt+C
    // and on signals that need no user code, then pick a thread that can
    // run (nullptr: none).
    static Thread* choose_next()
    {
        sint32_t s = end_screen;
        if (s >= 0)
        {
            end_screen = -1;
            end_programs_on((uint32_t)s);
        }
        s = pause_screen;
        if (s >= 0)
        {
            pause_screen = -1;
            pause_programs_on((uint32_t)s);
        }
        service_signals();
        return pick_next();
    }

    // Switch away from the current process (which may be blocked, stopped
    // or already terminated) to whatever should run next, the idle task if
    // nothing can. Returns once the process is resumed; a terminated process
    // never returns from here.
    static void schedule()
    {
        Thread* next = choose_next();
        if (!next)
            switch_kernel_task(&this_cpu()->idle_task);
        else if (next != cur_thread)
            switch_thread(next);

        // Running again: whoever switched to us made us current.
        slice_ticks = 0;
    }

    // schedule(), then deliver the signals the resumed process has to
    // handle into its trap frame `regs`/`iret`.
    static void reschedule(user_regs* regs, iret_frame* iret)
    {
        for (;;)
        {
            schedule();
            if (deliver_signals(current, regs, iret))
                continue;               // killed instead: pick again
            return;
        }
    }

    // -----------------------------------------------------------------------
    // Wait queues (wait.h)
    // -----------------------------------------------------------------------

    // Sleepers of sleep_until, woken by the timer once their tick is due.
    static wait_queue timer_wq;

    // Sleep on q, until wake_up, `tick` (0: no deadline) or a signal. False
    // only when a signal ended it: a resume after SIGSTOP/SIGCONT, say,
    // counts as a spurious wakeup and the caller re-checks.
    static bool queue_sleep(wait_queue* q, uint64_t tick)
    {
        Thread* t = cur_thread;

        uint64_t f = irq_save();
        t->woken     = false;
        t->wake_tick = tick;
        t->wq      = q;
        t->wq_next = q->head;
        q->head    = t;
        t->state   = TState::Blocked;
        irq_restore(f);

        schedule();

        // A signal ended the sleep: still on the queue.
        unlink_wait(t);
        bool woken = t->woken;
        t->woken = false;
        t->wake_tick = 0;
        return woken || !sig::next_deliverable(&t->proc->sig);
    }

    // Wake the sleepers on q that `due` accepts (all when due is null).
    static void queue_wake(wait_queue* q, bool (*due)(const Thread*))
    {
        uint64_t f = irq_save();
        Thread** link = &q->head;
        while (*link)
        {
            Thread* t = *link;
            if (due && !due(t))
            {
                link = &t->wq_next;
                continue;
            }
            *link = t->wq_next;
            t->wq = nullptr;
            t->wq_next = nullptr;
            t->woken = true;    // pick_next makes it runnable
        }
        irq_restore(f);
    }

    static bool tick_due(const Thread* t)
    {
        return pit::ticks() >= t->wake_tick;
    }

    void on_timer_tick()
    {
        if (timer_wq.head)
            queue_wake(&timer_wq, tick_due);
    }

    // The running thread's process has ended (on another CPU, while this
    // one ran it): the thread ends here. The last of such threads takes the
    // address space - and, if the process is gone, its slot - with it.
    // Never returns when it ends the thread.
    static void end_if_doomed(user_regs* regs, iret_frame* iret)
    {
        Thread* t = cur_thread;
        if (!t || !t->doomed)
            return;
        Process* p = t->proc;
        free_thread(t, SF_ABORTED);
        current = nullptr;
        if (count_threads(p) == 0)
        {
            release_address_space(p);
            if (p->reap_when_empty)
            {
                p->reap_when_empty = false;
                free_process(p);
            }
        }
        reschedule(regs, iret);
    }

    // Everything that has to happen on the way back to ring 3 when the
    // scheduler was not otherwise involved.
    static void return_to_user(user_regs* regs, iret_frame* iret)
    {
        end_if_doomed(regs, iret);

        if (!current)
        {
            reschedule(regs, iret);
            return;
        }

        if (service_signals() || !current || current->state != State::Live ||
            cur_thread->state != TState::Runnable)
        {
            reschedule(regs, iret);
            return;
        }

        if (deliver_signals(current, regs, iret))
            reschedule(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Session (console entry point)
    // -----------------------------------------------------------------------

    // vfs busy hook: a filesystem is in use while any live process has its
    // cwd inside it or holds an open fd on it. umount refuses then, because
    // it frees every vnode of the FS outright.
    static bool mount_in_use(mount* m)
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            const Process* p = &table[i];
            if (p->state == State::Unused)
                continue;
            if (p->cwd && p->cwd->mnt == m)
                return true;
            for (uint32_t r = 0; r < MAX_ROOTS; r++)
                if (p->roots[r].v && p->roots[r].v->mnt == m)
                    return true;
        }
        return filesys::any_open_on(m);
    }

    void init()
    {
        memory::memset((uint8_t*)table, 0x00, sizeof(table));
        vfs::set_busy_hook(mount_in_use);

        for (uint32_t s = 0; s < TERM_ALL_SCREENS; s++)
            input_owner[s] = screen_console[s] = -1;

        task::init(&this_cpu()->idle_task, "idle");

        asm volatile("fninit");
        fpu_save(fpu_template);
        // MXCSR (offset 24): all SSE exceptions masked, round to nearest.
        uint32_t mxcsr = 0x1F80;
        copy_bytes(fpu_template + 24, (const uint8_t*)&mxcsr, sizeof(mxcsr));
    }

    // launch flags
    const uint32_t LAUNCH_ADMIN   = 0x1;    // the admin right
    const uint32_t LAUNCH_CONSOLE = 0x2;    // a screen's console: no data folder

    // Load program `path` as a new process, not started yet (below).
    static Process* launch(const char* path, const ArgEnv* ae, vnode* const* arg_roots,
                           uint32_t flags = 0);

    // A kernel process: no user address space, runs `entry` on its task.
    static Process* start_kernel_process(const char* name, void (*entry)(void*))
    {
        Process* p = alloc_process();
        if (!p)
        {
            uart::printf("process: no memory for %s\n", name);
            screen::printf("\n\rprocess: no memory for %s", name);
            for (;;)
                asm volatile("cli; hlt");
        }

        // It works in the system cwd, not a per-process one.
        p->kernel = true;
        p->cr3    = paging::kernel_pml4();
        copy_name(p->name, name);
        if (p->cwd)
        {
            vfs::unref(p->cwd);
            p->cwd = nullptr;
        }
        Thread* t = first_thread(p);
        copy_bytes(t->fpu, fpu_template, sizeof(t->fpu));

        task::prepare_kernel(&t->task, name, kstack_top(t), entry, nullptr);
        start(p);
        return p;
    }


    // -----------------------------------------------------------------------
    // cmdkeeper: CMD.BIN on every shown screen
    // -----------------------------------------------------------------------

    static const char CMD_PATH[] = "/sfos/CMD.BIN";

    // A new CMD.BIN on screen s, with the admin right and no data folder. It
    // gets the screen's input - unless a program the last one started still
    // has it; that gives it back when it ends.
    static void start_cmd(uint32_t s)
    {
        Process* p = nullptr;
        ArgEnv ae;
        if (ae.init())
        {
            if (ae.push_kstr(false, "cmd") == 0)
                p = launch(CMD_PATH, &ae, nullptr, LAUNCH_ADMIN | LAUNCH_CONSOLE);
            ae.destroy();
        }
        if (!p)
        {
            uart::printf("cmdkeeper: cannot start %s on screen %u\n", CMD_PATH, s + 1);
            screen_console[s] = -2;
            return;
        }

        copy_name(p->name, "cmd");
        p->ppid   = 0;
        p->screen = s;
        Thread* t = first_thread(p);
        task::prepare_user(&t->task, p->name, kstack_top(t), &t->ctx);
        screen_console[s] = p->pid;
        start(p);
        if (!find_live(input_owner[s]))
            set_input_owner(s, p->pid);
        uart::printf("cmdkeeper: cmd pid %u on screen %u\n", (uint32_t)p->pid, s + 1);
    }

    static bool console_missing(void*)
    {
        for (uint32_t s = 0; s < TERM_SCREENS; s++)
            if (screen_console[s] == -1)
                return true;
        return false;
    }

    static void cmdkeeper_main(void*)
    {
        for (;;)
        {
            wait::wait_event(&keeper_wq, console_missing, nullptr, 0);
            for (uint32_t s = 0; s < TERM_SCREENS; s++)
                if (screen_console[s] == -1)
                    start_cmd(s);
            // One that ends as soon as it starts is not restarted more than
            // once a second.
            uint32_t hz = pit::real_frequency();
            wait::sleep_until(pit::ticks() + (hz ? hz : 1000));
        }
    }

    void start_cmdkeeper()
    {
        start_kernel_process("cmdkeeper", cmdkeeper_main);
    }

    void run_cpu()
    {
        task::init(&this_cpu()->idle_task, "idle");
        this_cpu()->thread = nullptr;
        this_cpu()->proc   = nullptr;
        bkl::enter();
        idle();
    }

    void idle()
    {
        // Interrupts arriving here come from ring 0, so they never re-enter
        // the scheduler; after each one, see whether somebody can run.
        for (;;)
        {
            Thread* next = choose_next();
            if (next)
                switch_thread(next);
            else
            {
                // Nobody runs on a free slot's stack now: hand those back
                // (a thread can end long after the console's program has).
                reclaim_kernel_stacks();

                // Halt without the big kernel lock: the interrupt that
                // wakes this CPU takes it on its own, and so does the next
                // round of this loop.
                bkl::leave();
                asm volatile("sti; hlt");
                bkl::enter();
            }
        }
    }

    // Load `path` with the collected argv/envp and allocate a process for
    // it, with its roots. ppid is left at 0; it is not started yet.
    static Process* launch(const char* path, const ArgEnv* ae, vnode* const* arg_roots,
                           uint32_t flags)
    {
        bool admin = flags & LAUNCH_ADMIN;
        Image img;
        if (load_program(path, vfs::cwd(), ae, &img, admin ? SDK_START_ADMIN : 0) < 0)
            return nullptr;

        Process* p = alloc_process();
        if (!p)
        {
            paging::destroy_address_space(img.cr3);
            kfree(img.args);
            return nullptr;
        }
        adopt_image(p, first_thread(p), &img, path);

        vnode* data = nullptr;
        vnode* tmp  = nullptr;
        if (!(flags & LAUNCH_CONSOLE))
            sffile::open_roots(p->name, &data, &tmp);
        if (data)
            add_root(p, "data", data);
        if (tmp)
            add_root(p, "tmp", tmp);
        // The admin right: disk:/ is the whole boot volume, mount:/ the rest.
        p->admin = admin;
        vnode* v = nullptr;
        if (admin && vfs::lookup("/", nullptr, &v, true) == 0)
            add_root(p, "disk", v);
        if (admin && vfs::lookup("/mount", nullptr, &v, true) == 0)
            add_root(p, "mount", v);
        // argN: what the console opened for argument N.
        for (uint32_t i = 1; arg_roots && i < ae->a_count; i++)
        {
            if (!arg_roots[i])
                continue;
            char name[8] = "arg";
            uint32_t n = 3;
            if (i >= 10)
                name[n++] = (char)('0' + i / 10 % 10);
            name[n++] = (char)('0' + i % 10);
            name[n] = '\0';
            vfs::ref(arg_roots[i]);
            add_root(p, name, arg_roots[i]);
        }
        return p;
    }

    // -----------------------------------------------------------------------
    // Handles to processes
    // -----------------------------------------------------------------------

    sint64_t open(handle_table* t, pid_t pid, uint32_t flags, sint32_t* out)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel || !p->obj)
            return -ESRCH;
        return handles::install(t, &p->obj->hdr, flags, 0, out);
    }

    // -----------------------------------------------------------------------
    // Background programs, the other half
    // -----------------------------------------------------------------------

    // Put p, not started yet, on hidden screen s with a log (its path into
    // log_name, 128 bytes; "" when there is none).
    static void to_background(Process* p, uint32_t s, char* log_name)
    {
        log_name[0] = '\0';
        p->screen = s;
        char file[64];
        screen_log[s].v   = create_log(p, file);
        screen_log[s].off = 0;
        if (screen_log[s].v)
        {
            // /files/<name>/<file>: what data:/ is (sffile.cpp).
            char* e = log_name;
            for (const char* c = "/files/"; *c; c++) *e++ = *c;
            for (const char* c = p->name; *c; c++)  *e++ = *c;
            *e++ = '/';
            for (const char* c = file; *c; c++)     *e++ = *c;
            *e = '\0';
        }
        uint32_t prev = term::selected();
        term::select(s);
        term::set_program(p->name);
        term::select(prev);
    }


    // -----------------------------------------------------------------------
    // Trap hooks
    // -----------------------------------------------------------------------

    // A program's weight: 2 on the shown screen, 1 anywhere else (another
    // screen, the background). Its thread's time slice is that many slices
    // long, so of two sharing a CPU, the one on screen gets twice the time.
    // It is looked at every tick: switching screens changes it at once.
    static inline uint32_t weight(const Process* p)
    {
        return p->screen == term::shown_screen() ? 2 : 1;
    }

    void on_user_interrupt(uint8_t irq, user_regs* regs, iret_frame* iret)
    {
        if (!current)
            return;
        end_if_doomed(regs, iret);

        if (screen_request())
        {
            reschedule(regs, iret);     // does not return
            return;
        }

        if (irq == IRQ0_TIMER && ++slice_ticks >= TIME_SLICE_TICKS * weight(current))
        {
            reschedule(regs, iret);
            return;
        }

        // Not a switch, but still a way back to ring 3: a ^C that arrived
        // while the process was spinning in user code gets acted on here.
        return_to_user(regs, iret);
    }

    // Map a CPU exception vector to the Linux signal a kernel would raise.
    static int vector_to_signal(uint64_t vector)
    {
        switch (vector)
        {
            case 0:  return SIGFPE;    // #DE divide error
            case 6:  return SIGILL;    // #UD invalid opcode
            case 13: return SIGSEGV;   // #GP general protection
            case 14: return SIGSEGV;   // #PF page fault
            default: return SIGSEGV;
        }
    }

    void on_user_fault(uint64_t vector, user_regs* regs, iret_frame* iret)
    {
        end_if_doomed(regs, iret);
        int n = vector_to_signal(vector);

        // A process can only survive its own fault if it asked to: there
        // has to be a handler, and the signal must not be blocked.
        // Delivering into a default or blocked disposition would re-run the
        // faulting instruction and land right back here.
        if (!sig::caught(&current->sig, n) ||
            (current->sig.blocked & SIGMASK(n)))
        {
            screen::printf("\n[pid %u %s terminated: CPU exception %u]\n",
                           (uint32_t)current->pid, current->name, (uint32_t)vector);

            terminate(current, signal_status(n));
            reschedule(regs, iret);
            return;
        }

        sig::post(&current->sig, n);
        return_to_user(regs, iret);
    }

    void syscall_enter(uint64_t nr, user_regs* regs, iret_frame* iret)
    {
        // A thread of a process that ended on another CPU goes no further.
        end_if_doomed(regs, iret);
        if (cur_thread)
        {
            cur_thread->syscall_nr = nr;
            cur_thread->restart_pending = false;
        }
    }

    void syscall_return(user_regs* regs, iret_frame* iret)
    {
        if (screen_request())
        {
            reschedule(regs, iret);
            return;
        }
        return_to_user(regs, iret);

        // Interrupted, but no handler ran after all (the signal went away
        // meanwhile): restart as if nothing had happened.
        if (cur_thread && cur_thread->restart_pending)
        {
            restart_syscall(cur_thread, regs, iret);
            cur_thread->restart_pending = false;
        }
    }

    // -----------------------------------------------------------------------
    // Hooks for sys_fs.cpp (file-descriptor syscalls)
    // -----------------------------------------------------------------------

    handle_table* cur_handles()
    {
        return current ? &current->handles : nullptr;
    }

    vnode* cur_root(const char* name)
    {
        if (!current)
            return nullptr;
        for (uint32_t i = 0; i < MAX_ROOTS; i++)
            if (current->roots[i].v && strcmp(current->roots[i].name, name) == 0)
                return current->roots[i].v;
        return nullptr;
    }

    vnode* cur_cwd()
    {
        return current ? current->cwd : nullptr;
    }

    void set_cwd(vnode* v)
    {
        if (!current)
            return;
        if (current->cwd == v)
            return;
        if (current->cwd)
            vfs::unref(current->cwd);
        current->cwd = v;       // takes the caller's reference
    }

    uint32_t cur_umask()
    {
        return current ? current->umask : 022;
    }

    void set_umask(uint32_t m)
    {
        if (current)
            current->umask = m & 0777;
    }

    // The tty asks before handing input to a reader: a background job that
    // reads from the terminal gets SIGTTIN instead of stealing the keys the
    // foreground job is waiting for.
    bool in_foreground()
    {
        if (!current || current->kernel)
            return true;                // the console owns the tty
        pid_t fgp = tty::fg_pgrp();
        return fgp == 0 || fgp == current->pgid;
    }

    pid_t cur_pgrp()
    {
        return current ? current->pgid : 0;
    }

    int signal_pgrp(pid_t pgid, int sig)
    {
        return signal_group(pgid, sig);
    }

    void syscall_interrupted(user_regs* regs)
    {
        // EINTR unless push_signal_frame (SA_RESTART) or syscall_return
        // (no handler) turn it into a restart.
        regs->rax = SYSCALL_ERR(EINTR);
        cur_thread->restart_pending = true;
    }

    // -----------------------------------------------------------------------
    // Syscalls: process control
    // -----------------------------------------------------------------------

    void sys_exit(user_regs* regs, iret_frame* iret)
    {
        // exit(code): the wait status carries (code & 0xff) << 8.
        terminate(current, exit_code_status((int)(sint32_t)regs->rdi));
        reschedule(regs, iret);
    }

    void sf_exit(user_regs* regs, iret_frame* iret)
    {
        // The exit code a parent's wait sees: 0 for success, else the low
        // byte of the status (never 0 for an error).
        uint64_t s = regs->rdi;
        int code = 0;
        if (SF_ERROR(s))
            code = (s & 0xFF) ? (int)(s & 0xFF) : 1;
        current->sf_status = s;
        terminate(current, exit_code_status(code));
        reschedule(regs, iret);
    }

    // exit and exit_group both end the whole process (the old ABI has no
    // threads of its own).
    void sys_exit_group(user_regs* regs, iret_frame* iret)
    {
        sys_exit(regs, iret);
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
        reschedule(regs, iret);
    }

    // nanosleep(req, rem): req/rem are user `struct timespec`. Sleeps in the
    // kernel (wait::sleep_until); rem is written only when a signal cuts the
    // sleep short.
    void sys_nanosleep(user_regs* regs, iret_frame*)
    {
        timespec req;
        if (!uaccess::copy_from_user(&req, regs->rdi, sizeof(req)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }
        uint64_t rem_ptr = regs->rsi;
        if (rem_ptr && !uaccess::writable(rem_ptr, sizeof(timespec)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }
        if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        uint64_t ms = (uint64_t)req.tv_sec * 1000ULL + (uint64_t)(req.tv_nsec / 1000000);
        if (ms == 0)
            ms = 1;             // never a busy spin

        uint32_t hz = pit::real_frequency();
        if (!hz)
            hz = pit::frequency();
        uint64_t ticks = (ms * hz + 999) / 1000;
        uint64_t deadline = pit::ticks() + (ticks ? ticks : 1);

        if (wait::sleep_until(deadline))
        {
            regs->rax = 0;
            return;
        }

        // A handler is about to run (a signal without one stops, kills or
        // is ignored without ending the sleep). Linux returns EINTR here
        // whatever SA_RESTART says, with the time that was left in rem.
        if (rem_ptr)
        {
            uint64_t now = pit::ticks();
            uint64_t left_ms = deadline > now ? ((deadline - now) * 1000) / hz : 0;
            timespec rem;
            rem.tv_sec  = (sint64_t)(left_ms / 1000);
            rem.tv_nsec = (sint64_t)(left_ms % 1000) * 1000000LL;
            if (!uaccess::copy_to_user(rem_ptr, &rem, sizeof(rem)))
            {
                regs->rax = SYSCALL_ERR(EFAULT);
                return;
            }
        }
        regs->rax = SYSCALL_ERR(EINTR);
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
        child->pgid        = current->pgid;     // same job as its parent
        child->screen      = current->screen;
        child->brk_start   = current->brk_start;
        child->brk         = current->brk;
        child->mmap_cursor = current->mmap_cursor;

        // POSIX: the child shares the parent's open file descriptions (same
        // offsets) and inherits its cwd and umask. alloc_process gave the
        // child the system cwd; replace it with the parent's.
        handles::fork(&child->handles, &current->handles);
        child->umask = current->umask;
        if (child->cwd)
            vfs::unref(child->cwd);
        child->cwd = current->cwd;
        if (child->cwd)
            vfs::ref(child->cwd);
        copy_bytes((uint8_t*)child->roots, (const uint8_t*)current->roots,
                   sizeof(child->roots));
        for (uint32_t r = 0; r < MAX_ROOTS; r++)
            if (child->roots[r].v)
                vfs::ref(child->roots[r].v);
        if (current->args && (child->args = (char*)kmalloc(current->args_size)))
        {
            copy_bytes((uint8_t*)child->args, (const uint8_t*)current->args,
                       current->args_size);
            child->args_size = current->args_size;
            child->argc      = current->argc;
        }

        // Handlers and the blocked mask carry over; pending signals do not
        // (POSIX: the child starts with an empty pending set).
        sig::inherit(&child->sig, &current->sig);
        first_thread(child)->restart_pending = false;
        child->mask_saved      = false;

        // The child resumes from the same instruction with rax = 0, on its
        // own kernel stack.
        Thread* ct = first_thread(child);
        ct->ctx.regs = *regs;
        ct->ctx.iret = *iret;
        ct->ctx.regs.rax = 0;
        fpu_save(ct->fpu);
        task::prepare_user(&ct->task, child->name, kstack_top(ct), &ct->ctx);

        start(child);
        regs->rax = (uint64_t)child->pid;
    }

    // execve(path, argv, envp). argv and envp are copied out of the *old*
    // address space before the new image is built; the old program keeps
    // running if anything fails.
    void sys_execve(user_regs* regs, iret_frame* iret)
    {
        // PATH_MAX does not go on the kernel stack: it is 64 KiB per process
        // and execve still has load_program and the ELF reader to call
        // underneath it.
        char* path = (char*)kmalloc(PATH_MAX);
        if (!path)
        {
            regs->rax = SYSCALL_ERR(ENOMEM);
            return;
        }
        sint64_t plen = uaccess::strncpy_from_user(path, regs->rdi, PATH_MAX);
        if (plen == -1)
        {
            kfree(path);
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }
        if (plen == -2)
        {
            kfree(path);
            regs->rax = SYSCALL_ERR(ENAMETOOLONG);
            return;
        }
        if (plen == 0)              // empty path
        {
            kfree(path);
            regs->rax = SYSCALL_ERR(ENOENT);
            return;
        }

        uint64_t argv_ptr = regs->rsi;
        uint64_t envp_ptr = regs->rdx;

        ArgEnv ae;
        if (!ae.init())
        {
            kfree(path);
            regs->rax = SYSCALL_ERR(ENOMEM);
            return;
        }

        int rc = 0;

        // --- argv ---
        if (!argv_ptr)
        {
            rc = ae.push_kstr(false, path);
        }
        else
        {
            for (int i = 0; rc == 0; i++)
            {
                uint64_t str = 0;
                if (!uaccess::copy_from_user(&str, argv_ptr + (uint64_t)i * 8, 8))
                {
                    rc = -EFAULT;
                    break;
                }
                if (!str)
                    break;
                rc = ae.push_user(false, str);
            }
            if (rc == 0 && ae.a_count == 0)
                rc = ae.push_kstr(false, path);
        }

        // --- envp ---
        if (rc == 0 && envp_ptr)
        {
            for (int i = 0; rc == 0; i++)
            {
                uint64_t str = 0;
                if (!uaccess::copy_from_user(&str, envp_ptr + (uint64_t)i * 8, 8))
                {
                    rc = -EFAULT;
                    break;
                }
                if (!str)
                    break;
                rc = ae.push_user(true, str);
            }
        }

        Image img;
        if (rc == 0)
        {
            sint64_t lr = load_program(path, current->cwd ? current->cwd
                                                          : vfs::cwd(),
                                       &ae, &img);
            rc = (lr < 0) ? (int)lr : 0;
        }
        ae.destroy();

        if (rc != 0)
        {
            kfree(path);
            regs->rax = SYSCALL_ERR(-rc);   // the old program keeps running
            return;
        }

        // Point of no return: swap in the new image.
        uint64_t old = current->cr3;
        paging::switch_address_space(img.cr3);
        paging::destroy_address_space(old);

        adopt_image(current, cur_thread, &img, path);   // copies the name it needs
        kfree(path);

        // execve keeps the fd table except CLOEXEC slots, and keeps cwd and
        // umask (POSIX). adopt_image reset only the address-space fields.
        handles::close_flagged(&current->handles, HANDLE_CLOEXEC);

        // Every handler address belonged to the image that has just been
        // replaced; ignored signals and the blocked mask survive.
        sig::reset_on_exec(&current->sig);
        cur_thread->restart_pending = false;
        current->mask_saved      = false;

        fpu_restore(cur_thread->fpu);
        *regs = cur_thread->ctx.regs;
        *iret = cur_thread->ctx.iret;
    }

    void sys_wait4(user_regs* regs, iret_frame* iret)
    {
        pid_t    pid        = (pid_t)(sint32_t)regs->rdi;
        uint64_t status_ptr = regs->rsi;
        uint64_t options    = regs->rdx;

        if (status_ptr && !uaccess::writable(status_ptr, sizeof(int)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        for (;;)
        {
            bool has_child = false;
            for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            {
                Process* q = &table[i];
                if (q->state == State::Unused || q->ppid != current->pid)
                    continue;
                if (pid > 0 && q->pid != pid)
                    continue;

                has_child = true;

                // A stopped or resumed child is reported without being reaped:
                // it is still there, and the shell will want to continue it.
                int  status = 0;
                bool report = false;

                if (q->state == State::Zombie)
                {
                    status = q->exit_status;
                    report = true;
                }
                else if (q->stop_pending && (options & WUNTRACED))
                {
                    status = q->stop_status;
                    report = true;
                }
                else if (q->cont_pending && (options & WCONTINUED))
                {
                    status = 0xFFFF;
                    report = true;
                }

                if (!report)
                    continue;

                if (status_ptr && !uaccess::copy_to_user(status_ptr, &status, sizeof(status)))
                {
                    regs->rax = SYSCALL_ERR(EFAULT);
                    return;
                }

                regs->rax = (uint64_t)q->pid;
                if (q->state == State::Zombie)
                    free_process(q);
                else
                {
                    q->stop_pending = false;
                    q->cont_pending = false;
                }
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

            // Sleep until a child event this call asked for, then scan again.
            // A caught signal ends the sleep: restart (SA_RESTART) or EINTR.
            current->wait_pid  = pid;
            current->wait_opts = options;
            if (!wait::wait_event(&current->child_wq, current_child_event, nullptr, 0))
            {
                syscall_interrupted(regs);
                return;
            }
        }
    }

    // kill(pid, sig) with the POSIX pid conventions. Nothing is delivered
    // here: the signal is posted, and syscall_return acts on it on the way
    // back to ring 3 - including when the target is the caller.
    void sys_kill(user_regs* regs, iret_frame*)
    {
        pid_t pid = (pid_t)(sint32_t)regs->rdi;
        int   n   = (int)(sint32_t)regs->rsi;

        // sig 0 delivers nothing and only reports whether the target exists.
        if (n < 0 || n >= NSIG)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        int count = 0;

        if (pid > 0)
        {
            Process* t = find_live(pid);
            if (t && !t->kernel)
            {
                count = 1;
                if (n)
                    post_signal(t, n);
            }
        }
        else if (pid == 0)
        {
            count = signal_group(current->pgid, n);
        }
        else if (pid == -1)
        {
            // Every process we may signal: all user processes but the
            // caller.
            for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            {
                Process* p = &table[i];
                if (!alive(p) || p->kernel || p == current)
                    continue;
                count++;
                if (n)
                    post_signal(p, n);
            }
        }
        else
        {
            count = signal_group(-pid, n);
        }

        regs->rax = count ? 0 : SYSCALL_ERR(ESRCH);
    }

    // -----------------------------------------------------------------------
    // Syscalls: signals
    // -----------------------------------------------------------------------

    // rt_sigaction(sig, act, oldact, sigsetsize). The structures crossing
    // the boundary are Linux's k_sigaction, so musl's own sigaction can sit
    // straight on top of this in stage 6.
    void sys_rt_sigaction(user_regs* regs, iret_frame*)
    {
        int      n       = (int)(sint32_t)regs->rdi;
        uint64_t act_ptr = regs->rsi;
        uint64_t old_ptr = regs->rdx;
        uint64_t setsize = regs->r10;

        if (!sig::valid(n) || n == SIGKILL || n == SIGSTOP ||
            setsize != SIGSET_BYTES)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        if (old_ptr &&
            !uaccess::copy_to_user(old_ptr, &current->sig.act[n],
                                   sizeof(k_sigaction)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        if (act_ptr)
        {
            k_sigaction ka;
            if (!uaccess::copy_from_user(&ka, act_ptr, sizeof(ka)))
            {
                regs->rax = SYSCALL_ERR(EFAULT);
                return;
            }

            // A handler with no trampoline could never return: the frame's
            // return address is what gets it back into the kernel.
            if (ka.handler != SIG_DFL && ka.handler != SIG_IGN && !ka.restorer)
            {
                regs->rax = SYSCALL_ERR(EINVAL);
                return;
            }

            ka.mask &= ~SIG_UNCATCHABLE;
            current->sig.act[n] = ka;

            // Setting a signal to ignore discards what is already pending;
            // otherwise it would be delivered the moment it is unignored.
            if (sig::discarded(&current->sig, n))
                sig::clear(&current->sig, n);
        }

        regs->rax = 0;
    }

    // rt_sigprocmask(how, set, oldset, sigsetsize)
    void sys_rt_sigprocmask(user_regs* regs, iret_frame*)
    {
        int      how     = (int)(sint32_t)regs->rdi;
        uint64_t set_ptr = regs->rsi;
        uint64_t old_ptr = regs->rdx;
        uint64_t setsize = regs->r10;

        if (setsize != SIGSET_BYTES)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }
        if (set_ptr && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        sigset_t old = current->sig.blocked;

        if (set_ptr)
        {
            sigset_t set;
            if (!uaccess::copy_from_user(&set, set_ptr, sizeof(set)))
            {
                regs->rax = SYSCALL_ERR(EFAULT);
                return;
            }
            sig::set_mask(&current->sig, how, set, nullptr);
        }

        if (old_ptr && !uaccess::copy_to_user(old_ptr, &old, sizeof(old)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        regs->rax = 0;
    }

    // rt_sigpending(set, sigsetsize)
    void sys_rt_sigpending(user_regs* regs, iret_frame*)
    {
        if (regs->rsi != SIGSET_BYTES)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }
        sigset_t pend = current->sig.pending;
        regs->rax = uaccess::copy_to_user(regs->rdi, &pend, sizeof(pend))
                        ? 0 : SYSCALL_ERR(EFAULT);
    }

    // Return from a handler: put back the context the frame saved. The only
    // syscall that does not go through the usual rax convention - it
    // restores rax along with everything else.
    void sys_rt_sigreturn(user_regs* regs, iret_frame* iret)
    {
        // The handler was entered with rsp at the frame; its `ret` popped
        // the trampoline address, so the frame starts one word below.
        uint64_t base = iret->rsp - 8;

        sig::frame f;
        if (!uaccess::copy_from_user(&f, base, sizeof(f)) || !sig::check_frame(&f))
        {
            screen::printf("\n[pid %u %s killed: corrupt signal frame]\n",
                           (uint32_t)current->pid, current->name);
            uart::printf("process: pid %u bad sigreturn frame at %llx\n",
                         (uint32_t)current->pid, base);
            terminate(current, signal_status(SIGSEGV));
            reschedule(regs, iret);
            return;
        }

        sig::set_mask(&current->sig, SIG_SETMASK, f.old_mask, nullptr);

        *regs = f.ctx.regs;
        *iret = f.ctx.iret;

        // The frame lives in memory ring 3 can write, so none of it is
        // trusted: the segments and the flags are the kernel's to set. A
        // forged rip or rsp is harmless - it faults in ring 3 like any
        // other bad address.
        iret->cs     = USER_CS;
        iret->ss     = USER_SS;
        iret->rflags = sig::sanitize_rflags(f.ctx.iret.rflags);

        cur_thread->restart_pending = false;
        current->mask_saved      = false;
    }

    // Nothing wakes this queue: only a signal that runs a handler ends a
    // sleep on it (one that stops, kills or is ignored does not).
    static wait_queue signal_wq;

    static void sleep_for_signal()
    {
        while (wait::sleep_on(&signal_wq))
            ;                               // spurious: SIGCONT after a stop
    }

    // pause(): wait for any signal that runs a handler.
    void sys_pause(user_regs* regs, iret_frame*)
    {
        sleep_for_signal();
        regs->rax = SYSCALL_ERR(EINTR);     // the only way pause returns
    }

    // rt_sigsuspend(mask, sigsetsize): swap the mask, wait, and let the
    // sigframe put the old one back - that is what makes it atomic.
    void sys_rt_sigsuspend(user_regs* regs, iret_frame*)
    {
        if (regs->rsi != SIGSET_BYTES)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        sigset_t mask;
        if (!uaccess::copy_from_user(&mask, regs->rdi, sizeof(mask)))
        {
            regs->rax = SYSCALL_ERR(EFAULT);
            return;
        }

        current->saved_mask = current->sig.blocked;
        current->mask_saved = true;
        sig::set_mask(&current->sig, SIG_SETMASK, mask, nullptr);

        sleep_for_signal();
        regs->rax = SYSCALL_ERR(EINTR);
    }

    // -----------------------------------------------------------------------
    // Syscalls: process groups and identity
    // -----------------------------------------------------------------------

    void sys_setpgid(user_regs* regs, iret_frame*)
    {
        pid_t pid  = (pid_t)(sint32_t)regs->rdi;
        pid_t pgid = (pid_t)(sint32_t)regs->rsi;

        if (pid < 0 || pgid < 0)
        {
            regs->rax = SYSCALL_ERR(EINVAL);
            return;
        }

        Process* t = pid ? find_live(pid) : current;
        if (!t)
        {
            regs->rax = SYSCALL_ERR(ESRCH);
            return;
        }
        // POSIX: only the process itself or its parent may move it.
        if (t != current && t->ppid != current->pid)
        {
            regs->rax = SYSCALL_ERR(ESRCH);
            return;
        }

        t->pgid = pgid ? pgid : t->pid;
        regs->rax = 0;
    }

    void sys_getpgid(user_regs* regs, iret_frame*)
    {
        pid_t pid = (pid_t)(sint32_t)regs->rdi;
        Process* t = pid ? find_live(pid) : current;
        regs->rax = t ? (uint64_t)t->pgid : SYSCALL_ERR(ESRCH);
    }

    void sys_getpgrp(user_regs* regs, iret_frame*)
    {
        regs->rax = (uint64_t)current->pgid;
    }

    // There are no sessions (the console runs one program at a time), so
    // setsid only detaches the caller into a group of its own.
    void sys_setsid(user_regs* regs, iret_frame*)
    {
        current->pgid = current->pid;
        regs->rax = (uint64_t)current->pid;
    }

    // No users yet: everything runs as root. These exist because the first
    // thing a ported program does is ask, and -ENOSYS makes it give up.
    void sys_getuid(user_regs* regs, iret_frame*)  { regs->rax = 0; }
    void sys_getgid(user_regs* regs, iret_frame*)  { regs->rax = 0; }
    void sys_geteuid(user_regs* regs, iret_frame*) { regs->rax = 0; }
    void sys_getegid(user_regs* regs, iret_frame*) { regs->rax = 0; }

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

        keyboard_event_t e;
        while (!tty::pop_key(&e))
        {
            if (!tty::wait_key())
            {
                syscall_interrupted(regs);
                return;
            }
        }

        regs->rax = uaccess::copy_to_user(regs->rdi, &e, sizeof(e)) ? 0 : SYSCALL_ERR(EFAULT);
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
        // Other CPUs running this space may still have them in their TLBs.
        // Nobody gets the frames before this returns: that takes the lock.
        smp::flush_tlb(read_cr3());
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
                    memory::memset((uint8_t*)phys_to_virt(frame), 0x00, PAGE_SIZE_4K);
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
                memory::memset((uint8_t*)phys_to_virt(frame), 0x00, PAGE_SIZE_4K);
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

    // SFCALL_PROCESS_START (Name, ArgCount, Args, *Handle, Flags): start
    // program /apps/<Name> with Args after its name, and hand back a handle
    // to it. The new process shares the caller's screen: it joins the
    // caller's process group (^C reaches both). Its ppid stays 0 - the
    // caller follows it through the handle, so it never lingers as a
    // zombie. SF_START_GIVE_INPUT hands it the caller's input, if the
    // caller has it.
    void sf_process_start(user_regs* regs, iret_frame*)
    {
        // Name: a program in /apps, or a path with a root ("disk:/x/y").
        char name[PATH_MAX];
        sint64_t len = uaccess::strncpy_from_user(name, regs->rdi, sizeof(name));
        bool rooted = false;
        for (sint64_t i = 0; len > 0 && i < len; i++)
            rooted |= name[i] == ':';
        bool ok = len > 0 && (rooted || len <= NAME_MAX);
        for (sint64_t i = 0; ok && !rooted && i < len; i++)
            ok = name[i] != '/';
        uint64_t argc = regs->rsi;
        uint64_t flags = regs->r8;
        if (!ok || argc > 256)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        if ((flags & SF_START_ADMIN) && !current->admin)
        {
            regs->rax = SF_ACCESS_DENIED;
            return;
        }

        char path[PATH_MAX];
        vnode* v = nullptr;
        if (rooted)
        {
            // Its VFS path, and its own name after the last slash.
            if (sffile::lookup(name, &v) != 0 || v->type != vtype::REG ||
                vfs::get_path(v, path, sizeof(path), nullptr) != 0)
            {
                if (v)
                    vfs::unref(v);
                regs->rax = SF_NOT_FOUND;
                return;
            }
            char* base = path;
            for (char* c = path; *c; c++)
                if (*c == '/')
                    base = c + 1;
            copy_bytes((uint8_t*)name, (const uint8_t*)base, strlen(base) + 1);
        }
        else
        {
            static const char prefix[] = "/apps/";
            copy_bytes((uint8_t*)path, (const uint8_t*)prefix, sizeof(prefix) - 1);
            copy_bytes((uint8_t*)path + sizeof(prefix) - 1, (const uint8_t*)name,
                       (uint64_t)len + 1);
            if (vfs::lookup(path, nullptr, &v, false) != 0)
            {
                regs->rax = SF_NOT_FOUND;
                return;
            }
        }
        vfs::unref(v);

        // ArgHandles (r9): argument i's file or folder, ~0 for none - the
        // new program's root arg<i+1>:. The caller keeps them open meanwhile.
        vnode* arg_roots[257] = {};
        for (uint64_t i = 0; regs->r9 && i < argc; i++)
        {
            uint64_t h = ~0ULL;
            if (!uaccess::copy_from_user(&h, regs->r9 + i * 8, 8))
            {
                regs->rax = SF_INVALID_PARAMETER;
                return;
            }
            sint64_t frc = 0;
            file* f = h < HANDLE_TABLE_SIZE
                    ? filesys::fd_get(&current->handles, (sint32_t)h, &frc) : nullptr;
            if (h != ~0ULL && !f)
            {
                regs->rax = SF_BAD_HANDLE;
                return;
            }
            arg_roots[i + 1] = f ? f->vn : nullptr;
        }

        ArgEnv ae;
        if (!ae.init())
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        int rc = ae.push_kstr(false, name);
        for (uint64_t i = 0; rc == 0 && i < argc; i++)
        {
            uint64_t str = 0;
            rc = uaccess::copy_from_user(&str, regs->rdx + i * 8, 8) ? ae.push_user(false, str)
                                                                     : -EFAULT;
        }
        // In the background: a hidden screen of its own, taken first.
        bool background = flags & SF_START_BACKGROUND;
        sint32_t hidden = background ? term::open_hidden() : 0;
        if (hidden < 0)
        {
            ae.destroy();
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        Process* p = rc == 0 ? launch(path, &ae, arg_roots,
                                      (flags & SF_START_ADMIN) ? LAUNCH_ADMIN : 0)
                             : nullptr;
        ae.destroy();
        if (!p)
        {
            if (background)
                term::close_hidden((uint32_t)hidden);
            regs->rax = rc == 0 || rc == -ENOMEM || rc == -E2BIG ? SF_OUT_OF_RESOURCES
                                                                 : SF_INVALID_PARAMETER;
            return;
        }

        p->pgid   = current->pgid;
        p->screen = current->screen;
        char log_name[128];
        if (background)
            to_background(p, (uint32_t)hidden, log_name);
        Thread* t = first_thread(p);
        task::prepare_user(&t->task, p->name, kstack_top(t), &t->ctx);
        start(p);                       // runs once this call is back in ring 3
        if (!background && (flags & SF_START_GIVE_INPUT) && owns_input(nullptr))
        {
            p->input_giver = current->pid;
            set_input_owner(p->screen, p->pid);
        }
        uart::printf("process: pid %u started %s (pid %u, cpu %u%s)\n",
                     (uint32_t)current->pid, p->name, (uint32_t)p->pid, p->cpu,
                     background ? ", in the background" : "");
        // What a screen's console runs, the log follows (the tests read it).
        if (current->pid == screen_console[current->screen])
        {
            if (background)
                uart::printf("console: background start, pid %u %s, screen %u, log %s\n",
                             (uint32_t)p->pid, p->name, p->screen, log_name);
            else
            {
                p->from_console = true;
                uart::printf("console: program start, pid %u %s cpu %u\n",
                             (uint32_t)p->pid, p->name, p->cpu);
            }
        }

        // No Handle: nobody follows it.
        regs->rax = SF_SUCCESS;
        if (!regs->r10)
            return;

        sint32_t h = -1;
        if (open(&current->handles, p->pid, 0, &h) != 0)
        {
            terminate(p, signal_status(SIGKILL));
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        uint64_t handle = (uint64_t)h;
        if (!uaccess::copy_to_user(regs->r10, &handle, sizeof(handle)))
        {
            handles::close(&current->handles, h);   // it runs on regardless
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
    }

    // SFCALL_PROCESS_WAIT (Handle, *Status): sleep until the process has
    // ended, store its SfStatus and close the handle.
    void sf_process_wait(user_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        sint32_t h  = regs->rdi < HANDLE_TABLE_SIZE ? (sint32_t)regs->rdi : -1;
        kobject* o  = handles::get(&current->handles, h, obj_type::Process, &rc);
        if (!o)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }

        // Our own reference: another thread may close the handle meanwhile.
        kobj::get(o);
        rc = objects::wait(o, 0);
        SfStatus status = ((proc_obj*)o)->sf_status;
        kobj::put(o);
        if (rc != 0)
        {
            regs->rax = rc == -EINTR ? SF_ABORTED : SF_BAD_HANDLE;
            return;
        }
        if (regs->rsi && !uaccess::copy_to_user(regs->rsi, &status, sizeof(status)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        handles::close(&current->handles, h);
        regs->rax = SF_SUCCESS;
    }

    // SFCALL_PROCESS_GET_ID (*Id).
    void sf_get_id(user_regs* regs, iret_frame*)
    {
        uint64_t id = (uint64_t)current->pid;
        regs->rax = uaccess::copy_to_user(regs->rdi, &id, sizeof(id))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    // SFCALL_PROCESS_GET_ARGS (Id, Buffer, *Size, *Count).
    void sf_get_args(user_regs* regs, iret_frame*)
    {
        Process* p = regs->rdi <= 0x7FFFFFFF ? find_live((pid_t)regs->rdi) : nullptr;
        if (!p || p->kernel)
        {
            regs->rax = SF_NOT_FOUND;
            return;
        }

        uint64_t size = 0;
        if (!uaccess::copy_from_user(&size, regs->rdx, sizeof(size)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        uint64_t need  = p->args_size;
        uint64_t count = p->argc;
        bool fits = size >= need;
        if (!uaccess::copy_to_user(regs->rdx, &need, sizeof(need)) ||
            (regs->r10 && !uaccess::copy_to_user(regs->r10, &count, sizeof(count))) ||
            (fits && need && !uaccess::copy_to_user(regs->rsi, p->args, need)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        regs->rax = fits ? SF_SUCCESS : SF_BUFFER_TOO_SMALL;
    }

    // SFCALL_MEMORY_ALLOCATE_PAGES (Count, *Address): zeroed pages,
    // read + write, from the mmap region.
    void sf_allocate_pages(user_regs* regs, iret_frame*)
    {
        uint64_t pages = regs->rdi;
        if (!pages || pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE_4K)
        {
            regs->rax = pages ? SF_OUT_OF_RESOURCES : SF_INVALID_PARAMETER;
            return;
        }
        uint64_t addr = find_free_range(current, pages);
        if (!addr)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        if (!map_user_region(addr, pages * PAGE_SIZE_4K, PAGE_WRITE | PAGE_NX))
        {
            release_range(addr, pages);
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        if (!uaccess::copy_to_user(regs->rsi, &addr, sizeof(addr)))
        {
            release_range(addr, pages);
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        current->mmap_cursor = addr + pages * PAGE_SIZE_4K;
        if (current->mmap_cursor >= USER_MMAP_LIMIT)
            current->mmap_cursor = USER_MMAP_BASE;
        regs->rax = SF_SUCCESS;
    }

    // SFCALL_MEMORY_FREE_PAGES (Address, Count).
    void sf_free_pages(user_regs* regs, iret_frame*)
    {
        uint64_t addr  = regs->rdi;
        uint64_t pages = regs->rsi;
        if (!pages || (addr & (PAGE_SIZE_4K - 1)) ||
            pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE_4K ||
            !range_in(addr, pages * PAGE_SIZE_4K, USER_MMAP_BASE, USER_MMAP_LIMIT))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        release_range(addr, pages);
        regs->rax = SF_SUCCESS;
    }

    // -----------------------------------------------------------------------
    // Threads (SfThread)
    // -----------------------------------------------------------------------

    // SFCALL_THREAD_CREATE (Entry, Arg, *Handle): a new thread of the
    // calling process. It starts in the SDK runtime (SdkHeader.ThreadStart,
    // which calls Entry(Arg)) on a stack of its own, and *Handle refers to
    // it for Join.
    void sf_thread_create(user_regs* regs, iret_frame*)
    {
        uint64_t entry = regs->rdi;
        uint64_t arg   = regs->rsi;
        uint64_t start = sdkpage::thread_start();
        if (!entry || !start)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }

        Thread* t = alloc_thread(current);
        if (!t)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        t->cpu = least_loaded_cpu();            // any CPU: the TLB is kept in step
        const uint64_t pages = THREAD_STACK_SIZE / PAGE_SIZE_4K;
        t->obj = thread_obj_new();
        uint64_t base = t->obj ? find_free_range(current, pages) : 0;
        if (!base || !map_user_region(base, THREAD_STACK_SIZE, PAGE_WRITE | PAGE_NX))
        {
            if (base)
                release_range(base, pages);
            free_thread(t, SF_ABORTED);
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        current->mmap_cursor = base + THREAD_STACK_SIZE;
        if (current->mmap_cursor >= USER_MMAP_LIMIT)
            current->mmap_cursor = USER_MMAP_BASE;
        t->stack_base  = base;
        t->stack_pages = pages;

        sint32_t h = -1;
        if (handles::install(&current->handles, &t->obj->hdr, 0, 0, &h) != 0)
        {
            release_range(base, pages);
            free_thread(t, SF_ABORTED);
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        uint64_t handle = (uint64_t)h;
        if (!uaccess::copy_to_user(regs->rdx, &handle, sizeof(handle)))
        {
            handles::close(&current->handles, h);
            release_range(base, pages);
            free_thread(t, SF_ABORTED);
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }

        // SdkThreadStart(Entry, Arg), the stack as right after a call.
        initial_context(&t->ctx, start, base + THREAD_STACK_SIZE - 8);
        t->ctx.regs.rdi = entry;
        t->ctx.regs.rsi = arg;
        copy_bytes(t->fpu, fpu_template, sizeof(t->fpu));
        task::prepare_user(&t->task, current->name, kstack_top(t), &t->ctx);
        t->state = TState::Runnable;
        regs->rax = SF_SUCCESS;
    }

    // SFCALL_THREAD_EXIT (SfStatus): the calling thread ends with that
    // status. The last thread of a process ends the process with it.
    void sf_thread_exit(user_regs* regs, iret_frame* iret)
    {
        if (count_threads(current) <= 1)
        {
            sf_exit(regs, iret);
            return;
        }
        Thread* t = cur_thread;
        if (t->stack_base)
            release_range(t->stack_base, t->stack_pages);
        free_thread(t, regs->rdi);
        reschedule(regs, iret);
    }

    // SFCALL_THREAD_JOIN (Handle, *Status): sleep until the thread has
    // ended, store its SfStatus and close the handle.
    void sf_thread_join(user_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        sint32_t h  = regs->rdi < HANDLE_TABLE_SIZE ? (sint32_t)regs->rdi : -1;
        kobject* o  = handles::get(&current->handles, h, obj_type::Thread, &rc);
        if (!o)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }

        // Our own reference: another thread may close the handle meanwhile.
        kobj::get(o);
        rc = objects::wait(o, 0);
        SfStatus status = ((thread_obj*)o)->status;
        kobj::put(o);
        if (rc != 0)
        {
            regs->rax = rc == -EINTR ? SF_ABORTED : SF_BAD_HANDLE;
            return;
        }
        if (regs->rsi && !uaccess::copy_to_user(regs->rsi, &status, sizeof(status)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        handles::close(&current->handles, h);
        regs->rax = SF_SUCCESS;
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
        smp::flush_tlb(read_cr3());         // permissions may have shrunk

        regs->rax = 0;
    }
} // namespace process

namespace wait
{
    bool sleep_on(wait_queue* q)
    {
        return process::queue_sleep(q, 0);
    }

    void wake_up(wait_queue* q)
    {
        process::queue_wake(q, nullptr);
    }

    bool sleep_until(uint64_t tick)
    {
        while (pit::ticks() < tick)
            if (!process::queue_sleep(&process::timer_wq, tick))
                return false;
        return true;
    }

    bool wait_event(wait_queue* q, bool (*cond)(void*), void* arg, uint64_t tick)
    {
        for (;;)
        {
            uint64_t f = process::irq_save();
            if (cond(arg) || (tick && pit::ticks() >= tick))
            {
                process::irq_restore(f);
                return true;
            }
            // Still with interrupts off: queue_sleep enqueues before any
            // IRQ can run, and this task keeps IF clear until it resumes.
            bool ok = process::queue_sleep(q, tick);
            process::irq_restore(f);
            if (!ok)
                return false;
        }
    }
}
