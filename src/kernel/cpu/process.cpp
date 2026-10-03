// Processes: table, ELF program loading, the scheduler, and the process and
// memory syscalls. See process.h for the scheduling model.
//
// User memory is W^X throughout: code is read+execute, everything writable
// (data, heap, stack) is NX. mmap/mprotect are the one sanctioned way to get
// RWX memory, which a JIT or tcc -run needs.

#include "../../include/cpu/process.h"
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
#include "../../include/errno.h"
#include "../../include/fs/openflags.h"

#define USER_CS             0x23
#define USER_SS             0x2B
#define RFLAGS_USER         0x202       // IF + reserved bit 1
#define TIME_SLICE_TICKS    10          // PIT ticks (~10 ms at 1 kHz), per unit of weight

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
    // Process state: Live (its threads run or sleep), Stopped (paused with
    // Ctrl+Alt+Z: its threads are not picked until it goes on), Ended (gone,
    // but its last thread still runs on another CPU: the slot waits for it).
    // Thread state: Runnable or Blocked (asleep on a wait queue).
    enum class State  : uint8_t { Unused, Live, Stopped, Ended };
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
                                    // it alone acts on end_requested
        bool        reap_when_empty;    // free the slot once its last (doomed)
                                        // thread is gone
        pid_t       pid;
        pid_t       ppid;           // who started it; 0 once that one is gone

        // A kernel process (cmdkeeper): no user address space, never
        // ended, runs kernel code on its task only.
        bool        kernel;
        SfStatus    sf_status;      // what SfMain or the last thread returned;
                                    // SF_ABORTED when something else ended it
        bool        end_requested;  // Ctrl+Alt+C, EndProcess: its CPU ends it

        char        name[32];

        // The screen it shows on and reads keys from, and who gave it that
        // screen's input when it owns it (input owners, below).
        uint32_t    screen;
        pid_t       input_giver;
        bool        console_raw;    // SF_CONSOLE_RAW (sfos/console.h)
        void*       line_history;   // ReadLine's earlier lines (sfconsole.cpp), kmalloc'ed
        vnode*      out;            // SF_START_OUTPUT: where its printing goes, referenced
        uint64_t    out_off;
        bool        admin;          // the admin right (sfos/admin.h)
        bool        from_console;   // started by its screen's console (the log says so)
        uint64_t    cpu_ticks;      // timer ticks its threads ran (account_tick)

        uint64_t    cr3;
        uint64_t    mmap_cursor;    // where the next AllocatePages search starts

        // Kernel objects the process holds, by handle. A file descriptor is a
        // handle holding a File.
        handle_table handles;

        // The process as a kernel object (what a handle to it refers to).
        // The slot holds one reference until the process exits.
        proc_obj*   obj;

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
        SfStatus   sf_status;   // how it ended (SfProcess Wait)
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
            o->sf_status   = SF_ABORTED;
            o->changed.head = nullptr;
            o->used        = true;
            return o;
        }
        return nullptr;
    }

    // The process behind p->obj is gone: record how, wake the waiters and
    // drop the slot's reference.
    static void proc_obj_exit(Process* p)
    {
        proc_obj* o = p->obj;
        if (!o)
            return;
        o->exited = true;
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

    // Ctrl+Alt+C (kbd.cpp): the screen whose programs are to end, -1
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
            p->sf_status = SF_ABORTED;
            p->cpu_ticks = 0;
            p->obj = proc_obj_new(p->pid);
            if (!p->obj)
            {
                free_thread(t, SF_ABORTED);
                return nullptr;         // slot stays Unused
            }
            handles::init(&p->handles);
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

    // Release the roots, the command line, ReadLine's history and the
    // output file.
    static void drop_roots(Process* p)
    {
        if (p->line_history)
            kfree(p->line_history);
        p->line_history = nullptr;
        if (p->out)
        {
            if (p->out->ops->fsync)
                p->out->ops->fsync(p->out);
            vfs::unref(p->out);
        }
        p->out = nullptr;
        p->out_off = 0;
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
        drop_roots(p);
        if (end_threads(p, SF_ABORTED))
        {
            // A thread still runs on another CPU: the slot goes with it.
            p->state = State::Ended;
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

    // A process that still exists: running, asleep or paused.
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

    // Ctrl+Alt+Z hands a screen's input to its console while its programs
    // are paused; who had it gets it back when they go on (-1: nobody).
    static pid_t paused_owner[TERM_ALL_SCREENS];

    // The title bar names the input owner of a shown screen.
    static void set_input_owner(uint32_t screen, pid_t pid)
    {
        input_owner[screen] = pid;
        wait::wake_up(&input_owner_wq);
        tty::wake_key_waiters();
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

    bool input_changed(uint32_t screen)
    {
        return !owns_input(nullptr) || current->screen != screen;
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

    static void request_end(Process* p);

    bool end_program(pid_t pid)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel)
            return false;
        request_end(p);
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

    void* line_history(uint64_t size)
    {
        if (!current)
            return nullptr;
        if (!current->line_history)
        {
            current->line_history = kmalloc(size);
            if (current->line_history)
                memory::memset((uint8_t*)current->line_history, 0, size);
        }
        return current->line_history;
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

    bool output_to_file(const char* s, uint64_t len)
    {
        if (!current || !current->out || current->console_raw)
            return false;
        uint64_t done = 0;
        if (len && current->out->ops->write(current->out, current->out_off, s, len, &done) == 0)
            current->out_off += done;
        return true;
    }

    // Hidden screen s, with its log, goes: nothing runs there any more.
    static void close_hidden_screen(uint32_t s)
    {
        ScreenLog* l = &screen_log[s];
        if (l->v)
        {
            if (l->v->ops->fsync)
                l->v->ops->fsync(l->v);
            vfs::unref(l->v);
        }
        l->v   = nullptr;
        l->off = 0;
        input_owner[s] = paused_owner[s] = -1;
        term::close_hidden(s);
    }

    // p crashed: `text` (one line) on its screen - on a line of its own -
    // into its log when it runs in the background, and (as all a screen
    // shows) into the kernel's.
    static void tell_crash(Process* p, const char* text)
    {
        uint64_t len = 0;
        while (text[len])
            len++;

        uint32_t prev = term::selected();
        term::select(p->screen);
        if (term::cursor_x())
            term::putc('\n');
        tty::write(text, len);
        term::select(prev);

        ScreenLog* l = &screen_log[p->screen];
        uint64_t done = 0;
        if (l->v && l->v->ops->write(l->v, l->off, text, len, &done) == 0)
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
        close_hidden_screen(s);
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

    // A command line being collected (Start, cmdkeeper): the strings live
    // in one growable kernel buffer, their offsets in a growable array. The
    // strings and their pointers together are capped at ARGS_MAX; more is
    // -E2BIG.
    const uint32_t ARGS_MAX = 128 * 1024;

    struct ArgList
    {
        char*     data;
        uint32_t  data_used;
        uint32_t  data_cap;

        uint32_t* a_off;        // string offsets into data
        uint32_t  a_count;
        uint32_t  a_cap;

        bool init()
        {
            data_used = a_count = 0;
            data_cap = 1024;  a_cap = 16;
            data  = (char*)kmalloc(data_cap);
            a_off = (uint32_t*)kmalloc(a_cap * sizeof(uint32_t));
            return data && a_off;
        }

        void destroy()
        {
            if (data)  kfree(data);
            if (a_off) kfree(a_off);
            data = nullptr; a_off = nullptr;
        }

        // Double the string buffer, never past ARGS_MAX. 0 or -errno.
        int grow_data()
        {
            if (data_cap >= ARGS_MAX)
                return -E2BIG;
            uint32_t nc = data_cap * 2;
            if (nc > ARGS_MAX)
                nc = ARGS_MAX;
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
        int finish(uint32_t len)
        {
            if ((uint64_t)data_used + len + 1 + ((uint64_t)a_count + 1) * 8 > ARGS_MAX)
                return -E2BIG;
            if (a_count == a_cap)
            {
                uint32_t nc = a_cap * 2;
                uint32_t* na = (uint32_t*)kmalloc(nc * sizeof(uint32_t));
                if (!na)
                    return -ENOMEM;
                copy_bytes((uint8_t*)na, (const uint8_t*)a_off, a_count * sizeof(uint32_t));
                kfree(a_off);
                a_off = na;
                a_cap = nc;
            }
            a_off[a_count++] = data_used;
            data[data_used + len] = '\0';
            data_used += len + 1;
            return 0;
        }

        // A kernel string.
        int push_kstr(const char* s)
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
            return finish(len);
        }

        // A string of the program's: read straight into the buffer, growing
        // it when it is cut.
        int push_user(uint64_t user_str)
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
                sint64_t r = uaccess::strncpy_from_user(data + data_used, user_str, room);
                if (r == -1)
                    return -EFAULT;
                if (r == -2)
                {
                    int g = grow_data();
                    if (g)
                        return g;
                    continue;
                }
                return finish((uint32_t)r);
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
    // the ELF segments, an empty stack and the SDK runtime with the start
    // info. The active address space is unchanged on return. Returns 0 or
    // -errno.
    static sint64_t load_program(const char* path, vnode* cwd,
                                 const ArgList* ae, Image* out, uint64_t sdk_flags = 0)
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

        elf::LoadResult lr = {0, 0, false};
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

    // p ends while its screen's console has the keys - paused, it was ended
    // with Ctrl+Alt+C: the last one gone, the title is the console's again.
    static void retitle_after(Process* p)
    {
        uint32_t s = p->screen;
        pid_t console = s < TERM_SCREENS ? screen_console[s] : -1;
        if (console < 0 || p->pid == console || input_owner[s] != console)
            return;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* q = &table[i];
            if (q != p && alive(q) && !q->kernel && q->screen == s && q->pid != console)
                return;
        }
        paused_owner[s] = -1;
        set_input_owner(s, console);
    }

    // Terminate `p` with `status` (what Wait tells): its threads, memory,
    // handles and roots go, the programs it started are its no more (ppid
    // 0), its screen and keys go back.
    static void terminate(Process* p, SfStatus status)
    {
        p->sf_status = status;
        uart::printf("process: pid %u %s ended, status %llx\n", (uint32_t)p->pid, p->name,
                     status);
        if (p->from_console)
            uart::printf("console: program end, status %llx\n", status);
        // Its threads end first: nothing of the process runs after this -
        // or, for one running on another CPU, after that CPU's next kernel
        // entry. The address space goes with the last of them.
        uint32_t left = end_threads(p, SF_ABORTED);
        if (!left)
            release_address_space(p);

        handles::close_all(&p->handles);
        drop_roots(p);

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (table[i].ppid == p->pid && &table[i] != p)
                table[i].ppid = 0;

        // A screen's console ended: cmdkeeper starts a new one. The programs
        // it started run on.
        if (p->screen < TERM_SCREENS && screen_console[p->screen] == p->pid)
        {
            screen_console[p->screen] = -1;
            wait::wake_up(&keeper_wq);
        }
        return_input(p);
        release_screen(p);
        retitle_after(p);

        // Handles to the process see the exit now. free_process keeps the
        // slot while a doomed thread still runs.
        proc_obj_exit(p);
        free_process(p);

        if (p == current)
            current = nullptr;
    }

    static bool wake_ready(Thread* t)
    {
        // A process to be ended stops waiting: its calls give up at once.
        return t->proc->end_requested || t->woken ||
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

    static void request_end(Process* p);
    static void pause_process(Process* p);
    static void resume_process(Process* p);

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
        uint32_t ended = 0;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen &&
                (!others || p->pid != screen_console[screen]))
            {
                request_end(p);
                ended++;
            }
        }

        // A clean screen saying what happened: whatever the programs left
        // there (a game's board, a half-drawn table) is of no use now.
        uint32_t prev = term::selected();
        term::select(screen);
        term::clear();
        screen::printf("Ctrl+Alt+C: %u program(s) ended\n", ended);
        term::select(prev);
    }

    // Ctrl+Alt+Z: the programs on the screen but its console pause - every
    // thread of each, whatever it is doing - or, when they are paused, go
    // on. The title bar says so.
    static void pause_programs_on(uint32_t screen)
    {
        bool any = false, paused = false;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen && p->pid != screen_console[screen])
            {
                any = true;
                paused |= p->state == State::Stopped;
            }
        }
        if (!any)
            return;

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p) && !p->kernel && p->screen == screen && p->pid != screen_console[screen])
            {
                if (paused)
                    resume_process(p);
                else
                    pause_process(p);
            }
        }

        // Paused, the screen's keys go to its console (fg, bg...) - the
        // title keeps the program's name; going on, back to who had them.
        pid_t console = screen_console[screen];
        Process* owner = find_live(input_owner[screen]);
        if (!paused && console >= 0 && owner && owner->screen == screen &&
            owner->pid != console)
        {
            paused_owner[screen] = owner->pid;
            input_owner[screen]  = console;
            wait::wake_up(&input_owner_wq);
            tty::wake_key_waiters();
        }
        else if (paused)
        {
            Process* had = find_live(paused_owner[screen]);
            paused_owner[screen] = -1;
            if (had && had->screen == screen)
            {
                // Off the console's prompt: the program goes on below it.
                uint32_t prev = term::selected();
                term::select(screen);
                if (term::cursor_x())
                    term::putc('\n');
                term::select(prev);
                set_input_owner(screen, had->pid);
            }
        }
        term::set_paused(screen, !paused);
        uart::printf("console: screen %u %s\n", screen, paused ? "goes on" : "paused");
    }

    // -----------------------------------------------------------------------
    // Ending and pausing
    // -----------------------------------------------------------------------
    //
    // A program is ended (Ctrl+Alt+C, EndProcess) on request: its home CPU
    // acts on it at its next scheduling decision, as it may be running
    // there right now; a thread of it running on another CPU ends at that
    // CPU's next kernel entry (end_threads). Pausing takes effect at once:
    // the threads of a stopped process are not picked, and one running now
    // is switched away at its next tick.

    static void request_end(Process* p)
    {
        if (alive(p) && !p->kernel)
            p->end_requested = true;
    }

    static void pause_process(Process* p)
    {
        if (p->state == State::Live && !p->kernel)
            p->state = State::Stopped;
    }

    static void resume_process(Process* p)
    {
        if (p->state == State::Stopped)
            p->state = State::Live;
    }

    // End the processes of this CPU that are to be ended. Returns true when
    // `current` can no longer continue and the caller has to reschedule.
    static bool service_requests()
    {
        bool switch_away = false;
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->cpu != this_cpu()->index || !alive(p) || !p->end_requested)
                continue;               // its own CPU acts on it (it may be running there)
            switch_away |= (p == current);
            terminate(p, SF_ABORTED);
        }
        return switch_away;
    }

    // The system-wide part of every scheduling decision: act on Ctrl+Alt+C
    // and Ctrl+Alt+Z and on the processes to be ended, then pick a thread
    // that can run (nullptr: none).
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
        service_requests();
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

    // Switch away at a way back to ring 3 (a call's end, an interrupt): the
    // trap frame on the kernel stack is what the process resumes from.
    static void reschedule(user_regs*, iret_frame*)
    {
        schedule();
    }

    // -----------------------------------------------------------------------
    // Wait queues (wait.h)
    // -----------------------------------------------------------------------

    // Sleepers of sleep_until, woken by the timer once their tick is due.
    static wait_queue timer_wq;

    // Sleep on q, until wake_up, `tick` (0: no deadline) or an end request.
    // False only when the process is to be ended: a pause and going on,
    // say, counts as a spurious wakeup and the caller re-checks.
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

        // An end request ended the sleep: still on the queue.
        unlink_wait(t);
        bool woken = t->woken;
        t->woken = false;
        t->wake_tick = 0;
        return woken || !t->proc->end_requested;
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

    // Ticks each CPU has run, and of them those it worked (not idle).
    static uint64_t cpu_total[acpi::MAX_CPUS];
    static uint64_t cpu_busy[acpi::MAX_CPUS];

    void account_tick()
    {
        Cpu* c = this_cpu();
        if (c->index >= acpi::MAX_CPUS)
            return;
        cpu_total[c->index]++;
        if (!c->thread)
            return;                     // idle
        cpu_busy[c->index]++;
        if (c->proc)
            __atomic_add_fetch(&c->proc->cpu_ticks, 1, __ATOMIC_RELAXED);
    }

    static uint64_t ticks_to_ms(uint64_t ticks)
    {
        uint32_t hz = pit::real_frequency();
        if (!hz)
            hz = pit::frequency();
        return hz ? ticks * 1000 / hz : ticks;
    }

    void cpu_times(uint32_t cpu, uint64_t* busy_ms, uint64_t* total_ms)
    {
        bool ok = cpu < acpi::MAX_CPUS;
        *busy_ms  = ok ? ticks_to_ms(cpu_busy[cpu]) : 0;
        *total_ms = ok ? ticks_to_ms(cpu_total[cpu]) : 0;
    }

    bool program_stats(pid_t pid, SfProcessStats* out)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel)
            return false;
        memory::memset((uint8_t*)out, 0, sizeof(*out));
        Process* parent = p->ppid ? find_live(p->ppid) : nullptr;
        out->Id       = (uint64_t)p->pid;
        out->ParentId = parent && !parent->kernel ? (uint64_t)parent->pid : 0;
        out->CpuTime  = ticks_to_ms(p->cpu_ticks);
        out->Memory   = p->cr3 ? paging::count_user_pages(p->cr3) * PAGE_SIZE_4K : 0;
        out->Threads  = count_threads(p);
        out->Screen   = p->screen < TERM_SCREENS ? p->screen + 1 : 0;
        out->Flags    = (p->state == State::Stopped ? SF_PROCESS_PAUSED : 0) |
                        (p->admin ? SF_PROCESS_ADMIN : 0);
        copy_bytes((uint8_t*)out->Name, (const uint8_t*)p->name, sizeof(out->Name) - 1);
        return true;
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

        if (service_requests() || !current || current->state != State::Live ||
            cur_thread->state != TState::Runnable)
            reschedule(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Starting up
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
            input_owner[s] = screen_console[s] = paused_owner[s] = -1;

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
    static Process* launch(const char* path, const ArgList* ae, vnode* const* arg_roots,
                           uint32_t flags = 0);

    // A kernel process: no user address space, runs `entry` on its task.
    Process* start_kernel_process(const char* name, void (*entry)(void*))
    {
        Process* p = alloc_process();
        if (!p)
        {
            uart::printf("process: no memory for %s\n", name);
            screen::printf("\n\rprocess: no memory for %s", name);
            for (;;)
                asm volatile("cli; hlt");
        }

        p->kernel = true;
        p->cr3    = paging::kernel_pml4();
        copy_name(p->name, name);
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
        ArgList ae;
        if (ae.init())
        {
            if (ae.push_kstr("cmd") == 0)
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
    static Process* launch(const char* path, const ArgList* ae, vnode* const* arg_roots,
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

    sint64_t open(handle_table* t, pid_t pid, sint32_t* out)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel || !p->obj)
            return -ESRCH;
        return handles::install(t, &p->obj->hdr, out);
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


    // fg and bg (sfos/admin.h): a program moves to another screen together
    // with everything else on its screen but the console - it and what it
    // started share one screen - and what the screen shows goes along.

    // Is p one of the programs of screen s that move?
    static inline bool moves(const Process* p, uint32_t s)
    {
        return alive(p) && !p->kernel && p->screen == s && p->pid != screen_console[s];
    }

    // Move the programs of screen `from` to `to`. Returns which of them had
    // the keys there (-1: none); `from` is left to its console, or closed
    // when it was a hidden one.
    static pid_t move_programs(uint32_t from, uint32_t to)
    {
        Process* o = find_live(input_owner[from]);
        if (!o || !moves(o, from))
            o = find_live(paused_owner[from]);
        pid_t owner = o && moves(o, from) ? o->pid : -1;

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (moves(&table[i], from))
                table[i].screen = to;
        term::copy_screen(from, to);

        if (from >= TERM_SCREENS)
            close_hidden_screen(from);
        else
        {
            paused_owner[from] = -1;
            if (owner >= 0 || !find_live(input_owner[from]))
                set_input_owner(from, screen_console[from] >= 0 ? screen_console[from] : -1);
            term::set_paused(from, false);
        }
        tty::wake_key_waiters();        // a ReadLine goes on waiting there
        return owner;
    }

    // Let the programs of screen s go on, if paused.
    static void go_on(uint32_t s)
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (moves(&table[i], s))
                resume_process(&table[i]);
        term::set_paused(s, false);
    }

    SfStatus move_to_foreground(pid_t pid)
    {
        Process* p = find_live(pid);
        uint32_t to = current->screen;
        if (!p || p->kernel)
            return SF_NOT_FOUND;
        if (p->pid == screen_console[p->screen] || to >= TERM_SCREENS)
            return SF_ACCESS_DENIED;

        pid_t owner = -1;
        uint32_t from = p->screen;
        if (from != to)
        {
            // Only onto a screen with nothing else on it.
            for (uint32_t i = 0; i < MAX_PROCESSES; i++)
                if (moves(&table[i], to) && &table[i] != current)
                    return SF_IN_USE;
            owner = move_programs(from, to);
        }
        else
        {
            Process* o = find_live(paused_owner[to]);
            paused_owner[to] = -1;
            if (o && moves(o, to))
                owner = o->pid;
        }
        if (owner < 0)
            owner = pid;

        go_on(to);
        find_live(owner)->input_giver = current->pid;
        set_input_owner(to, owner);
        uart::printf("console: fg pid %u %s from screen %u to %u\n", (uint32_t)pid, p->name,
                     from, to);
        return SF_SUCCESS;
    }

    SfStatus move_to_background(pid_t pid)
    {
        Process* p = find_live(pid);
        if (!p || p->kernel)
            return SF_NOT_FOUND;
        if (p->pid == screen_console[p->screen])
            return SF_ACCESS_DENIED;

        uint32_t from = p->screen;
        if (from >= TERM_SCREENS)
        {
            go_on(from);                // there already
            return SF_SUCCESS;
        }
        sint32_t hidden = term::open_hidden();
        if (hidden < 0)
            return SF_OUT_OF_RESOURCES;
        pid_t owner = move_programs(from, (uint32_t)hidden);
        char log_name[128];
        to_background(p, (uint32_t)hidden, log_name);
        input_owner[hidden] = owner;    // it reads there once it is back
        go_on((uint32_t)hidden);
        uart::printf("console: bg pid %u %s from screen %u to %u, log %s\n", (uint32_t)pid,
                     p->name, from, (uint32_t)hidden, log_name);
        return SF_SUCCESS;
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

        // Not a switch, but still a way back to ring 3: a Ctrl+Alt+C that
        // arrived while the program was spinning in user code acts here.
        return_to_user(regs, iret);
    }

    // A fault in ring 3 ends the program - never the machine - saying why
    // and where on its screen (tell_crash).
    void on_user_fault(uint64_t vector, user_regs* regs, iret_frame* iret)
    {
        end_if_doomed(regs, iret);

        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        char text[160];
        screen::capture(text, sizeof(text));
        screen::printf("[%u] %s crashed: ", (uint32_t)current->pid, current->name);
        switch (vector)
        {
            case 0:  screen::printf("division by zero");                  break;
            case 6:  screen::printf("invalid instruction");               break;
            case 13: screen::printf("general protection fault");          break;
            case 14: screen::printf("page fault at address %llx", cr2);   break;
            default: screen::printf("CPU exception %u", (uint32_t)vector); break;
        }
        screen::printf(", instruction at %llx\n", iret->rip);
        screen::end_capture();
        tell_crash(current, text);

        terminate(current, SF_CRASHED);
        reschedule(regs, iret);
    }

    void syscall_enter(user_regs* regs, iret_frame* iret)
    {
        // A thread of a process that ended on another CPU goes no further.
        end_if_doomed(regs, iret);
    }

    void syscall_return(user_regs* regs, iret_frame* iret)
    {
        if (screen_request())
        {
            reschedule(regs, iret);
            return;
        }
        return_to_user(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Hooks for the file calls (sffile.cpp, fileio.cpp)
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

    // -----------------------------------------------------------------------
    // Syscalls: process control
    // -----------------------------------------------------------------------

    void sf_exit(user_regs* regs, iret_frame* iret)
    {
        terminate(current, regs->rdi);
        reschedule(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Syscalls: signals
    // -----------------------------------------------------------------------

    // Nothing wakes this queue: only a signal that runs a handler ends a
    // sleep on it (one that stops, kills or is ignored does not).
    static wait_queue signal_wq;

    // -----------------------------------------------------------------------
    // Syscalls: process groups and identity
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Syscalls: keyboard
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Syscalls: memory
    // -----------------------------------------------------------------------

    static bool range_in(uint64_t addr, uint64_t len, uint64_t lo, uint64_t hi)
    {
        return addr >= lo && addr < hi && len <= hi - addr;
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
        // SF_START_OUTPUT: one more, the file its printing goes to, from
        // that handle's position on.
        vnode* arg_roots[257] = {};
        vnode* out = nullptr;
        uint64_t out_off = 0;
        if (flags & SF_START_OUTPUT)
        {
            uint64_t h = ~0ULL;
            sint64_t frc = 0;
            file* f = regs->r9 && uaccess::copy_from_user(&h, regs->r9 + argc * 8, 8) &&
                      h < HANDLE_TABLE_SIZE
                    ? filesys::fd_get(&current->handles, (sint32_t)h, &frc) : nullptr;
            if (!f || f->vn->type != vtype::REG)
            {
                regs->rax = SF_BAD_HANDLE;
                return;
            }
            out = f->vn;
            out_off = f->offset;
        }
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

        ArgList ae;
        if (!ae.init())
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        int rc = ae.push_kstr(name);
        for (uint64_t i = 0; rc == 0 && i < argc; i++)
        {
            uint64_t str = 0;
            rc = uaccess::copy_from_user(&str, regs->rdx + i * 8, 8) ? ae.push_user(str)
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

        p->screen = current->screen;
        if (out)
        {
            vfs::ref(out);
            p->out = out;
            p->out_off = out_off;
        }
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
        if (open(&current->handles, p->pid, &h) != 0)
        {
            terminate(p, SF_ABORTED);
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

    // SFCALL_PROCESS_ID_OF (Handle, *Id): the process behind a handle.
    void sf_process_id_of(user_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        sint32_t h  = regs->rdi < HANDLE_TABLE_SIZE ? (sint32_t)regs->rdi : -1;
        kobject* o  = handles::get(&current->handles, h, obj_type::Process, &rc);
        if (!o)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        uint64_t id = (uint64_t)((proc_obj*)o)->pid;
        regs->rax = uaccess::copy_to_user(regs->rsi, &id, sizeof(id))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
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
        if (handles::install(&current->handles, &t->obj->hdr, &h) != 0)
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
