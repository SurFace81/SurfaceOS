// Processes: table, ELF program loading, the scheduler, and the process and
// memory syscalls. See process.h for the scheduling model.
//
// User memory is W^X throughout: code is read+execute, everything writable
// (data, heap, stack) is NX. mmap/mprotect are the one sanctioned way to get
// RWX memory, which a JIT or tcc -run needs.

#include "../../include/cpu/process.h"
#include "../../include/cpu/signal.h"
#include "../../include/cpu/tss.h"
#include "../../include/cpu/elf.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/irq.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/tty.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/time.h"
#include "../../sdk/include/abi/auxv.h"
#include "../../sdk/include/abi/fcntl.h"

extern "C" void process_enter_user(cpu_context* ctx);
extern "C" void process_return_to_kernel(void);

#define USER_CS             0x23
#define USER_SS             0x2B
#define RFLAGS_USER         0x202       // IF + reserved bit 1
#define INT80_LENGTH        2           // `int $0x80` is CD 80
#define TIME_SLICE_TICKS    10          // PIT ticks (~10 ms at 1 kHz)

// Syscall failure: -errno in rax (Linux convention, see abi/errno.h).
#define SYSCALL_ERR(e)      ((uint64_t)(sint64_t)-(e))

namespace process
{
    // -----------------------------------------------------------------------
    // Process table
    // -----------------------------------------------------------------------

    enum class State : uint8_t { Unused, Runnable, Blocked, Stopped, Zombie };
    enum class Wait  : uint8_t { None, Key, Child, Sleep, Signal };

    struct Process
    {
        State       state;
        Wait        wait;
        pid_t       pid;
        pid_t       ppid;           // 0: parent is the console session
        pid_t       pgid;           // process group, for job control
        pid_t       wait_pid;       // Wait::Child: which child (<= 0: any)
        uint64_t    wait_opts;      // Wait::Child: WNOHANG/WUNTRACED/...
        uint64_t    wake_tick;      // Wait::Sleep
        int         exit_status;    // valid in Zombie

        // Job control: set when the process is stopped, cleared when the
        // parent reports it. cont_pending does the same for SIGCONT.
        int         stop_status;
        bool        stop_pending;   // a stop the parent has not seen yet
        bool        cont_pending;   // ditto for a resume

        // Signals. `rewound` says the saved context points at an `int 0x80`
        // that block() rewound and that has not re-executed yet - which is
        // exactly when a caught signal has to choose between EINTR and
        // SA_RESTART. It is cleared the moment the syscall does run again
        // (syscall_enter), so it can never be stale.
        sig::signal_state sig;
        bool        rewound;
        bool        mask_saved;     // sigsuspend: restore this on sigreturn
        sigset_t    saved_mask;

        char        name[32];

        uint64_t    cr3;
        uint64_t    brk_start;      // end of the ELF image
        uint64_t    brk;            // current program break
        uint64_t    mmap_cursor;    // where the next mmap search starts

        cpu_context ctx;            // user state while not running
        uint8_t     fpu[512] __attribute__((aligned(16)));   // FXSAVE area
        // Kernel stack for traps taken while this process runs (TSS RSP0).
        // Owned by the table *slot*, not by the process: terminate() can
        // free a process while running on this very stack, so it is released
        // only at session teardown, from the console stack.
        uint64_t    kstack;     // base (direct-map virtual), 0 when the slot has none

        // POSIX file state: descriptors, cwd (referenced vnode) and umask.
        fd_table    fds;
        vnode*      cwd;
        uint32_t    umask;

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

    const uint64_t KERNEL_STACK_FRAMES = KERNEL_STACK_SIZE / 4096;

    // Clean FPU/SSE state every new program starts from.
    static uint8_t fpu_template[512] __attribute__((aligned(16)));

    static inline uint64_t read_cr3()
    {
        uint64_t v;
        asm volatile("mov %%cr3, %0" : "=r"(v));
        return v;
    }

    static inline uint64_t kstack_top(const Process* p)
    {
        return p->kstack + KERNEL_STACK_SIZE;
    }

    // Hand every live slot's kernel stack back. Only safe once nothing runs
    // on one of them, i.e. after process_return_to_kernel has put us back on
    // the console stack.
    static void free_kernel_stacks()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (table[i].kstack)
            {
                pmm::free_frames(virt_to_phys((void*)table[i].kstack), KERNEL_STACK_FRAMES);
                table[i].kstack = 0;
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

    static Process* alloc_process()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            if (table[i].state != State::Unused)
                continue;

            Process* p = &table[i];
            uint64_t ks = p->kstack;    // belongs to the slot; survives reuse
            memory::memset((uint8_t*)p, 0x00, sizeof(Process));
            p->kstack = ks;
            if (!p->kstack)
            {
                uint64_t frames = pmm::alloc_frames(KERNEL_STACK_FRAMES);
                if (!frames)
                    return nullptr;     // slot stays Unused
                p->kstack = (uint64_t)phys_to_virt(frames);
            }
            p->pid = next_pid++;
            if (next_pid <= 0)
                next_pid = 1;
            p->pgid = p->pid;           // its own group until setpgid says otherwise
            sig::init(&p->sig);

            filesys::fdtable_init(&p->fds);
            p->umask = 022;
            p->cwd = vfs::cwd_ref();        // inherit the system cwd
            return p;
        }
        return nullptr;
    }

    static void free_process(Process* p)
    {
        filesys::fdtable_close_all(&p->fds);
        if (p->cwd)
        {
            vfs::unref(p->cwd);
            p->cwd = nullptr;
        }
        p->state = State::Unused;
        p->pid = 0;
    }

    // A process that still exists and can be signalled. A stopped process
    // counts: SIGCONT is the whole point of it being there.
    static inline bool alive(const Process* p)
    {
        return p->state == State::Runnable || p->state == State::Blocked ||
               p->state == State::Stopped;
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
    // Session keyboard input (the line discipline itself lives in tty.cpp)
    // -----------------------------------------------------------------------

    // Runs in the keyboard IRQ: forwards to the tty ring. The tty decides
    // what interrupts a session (Ctrl+C, c_cc[VINTR]); Esc used to do it
    // here, which meant an application could never see Esc or any escape
    // sequence built on it.
    static void session_key_handler(keyboard_event_t e)
    {
        tty::on_key(e);
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
        // The trap we are about to return from still runs on the outgoing
        // process's stack - that is fine, it is finished with. What matters
        // is that the *next* entry from ring 3 lands on p's own stack.
        tss::set_kernel_stack(kstack_top(p));
        paging::switch_address_space(p->cr3);
        fpu_restore(p->fpu);
        *regs = p->ctx.regs;
        *iret = p->ctx.iret;
    }

    static void initial_context(cpu_context* ctx, uint64_t entry, uint64_t rsp)
    {
        memory::memset((uint8_t*)ctx, 0x00, sizeof(cpu_context));
        ctx->iret.rip    = entry;
        ctx->iret.cs     = USER_CS;
        ctx->iret.rflags = RFLAGS_USER;
        // The SysV ABI initial-process stack: rsp points at argc and is
        // 16-byte aligned. _start pops argc from there.
        ctx->iret.rsp    = rsp;
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
        uint64_t rsp;           // initial stack pointer (points at argc)
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

    // AT_RANDOM content. No entropy source exists yet (stage 6 plans
    // getrandom); musl only needs *something* stable for its stack canary.
    static const uint8_t random_bytes[16] =
        { 0x53, 0x75, 0x72, 0x66, 0x61, 0x63, 0x65, 0x4F,
          0x53, 0x2D, 0x61, 0x74, 0x72, 0x6E, 0x64, 0x31 };

    // Build the SysV ABI initial process stack (the exact layout Linux
    // uses, low addresses first):
    //
    //   rsp -> argc                      <- 16-byte aligned
    //          argv[0..argc-1], NULL
    //          envp[0..], NULL
    //          auxv entries ... AT_NULL
    //          (alignment padding)
    //          16 random bytes (AT_RANDOM)
    //          argv/envp strings
    //   high ->  USER_STACK_TOP
    //
    // Everything is staged in a kernel buffer and then copied through the
    // direct map, so the stack can stay mapped as it is. Returns the
    // future rsp in *out_rsp, or a negative errno.
    static sint64_t build_initial_stack(const ArgEnv* ae, const elf::LoadResult* lr,
                                        uint64_t* out_rsp)
    {
        uint32_t strings_size = ae->data_used;
        uint32_t n_aux = (lr->phdr_vaddr ? 1u : 0u) + 5;  // PHENT PHNUM PAGESZ ENTRY RANDOM
        uint32_t auxv_size = (n_aux + 1) * 16;            // + AT_NULL
        uint32_t envp_size = (ae->e_count + 1) * 8;
        uint32_t argv_size = (ae->a_count + 1) * 8;

        uint32_t fixed = 8 + argv_size + envp_size + auxv_size;
        uint32_t pad   = (16 - (fixed % 16)) % 16;
        uint32_t total = fixed + pad + 16 + strings_size;
        total = (total + 15) & ~(uint32_t)15;

        uint8_t* buf = (uint8_t*)kmalloc(total);
        if (!buf)
            return -ENOMEM;
        memory::memset(buf, 0x00, total);

        uint64_t base       = USER_STACK_TOP - total;
        uint32_t random_off = fixed + pad;
        uint32_t strings_off = random_off + 16;
        uint32_t off = 0;

        // --- argc ---
        uint64_t argc = ae->a_count;
        copy_bytes(buf + off, (const uint8_t*)&argc, 8);
        off += 8;

        // --- argv pointers, NULL-terminated ---
        for (uint32_t i = 0; i < ae->a_count; i++)
        {
            uint64_t v = base + strings_off + ae->a_off[i];
            copy_bytes(buf + off, (const uint8_t*)&v, 8);
            off += 8;
        }
        off += 8;

        // --- envp pointers, NULL-terminated ---
        for (uint32_t i = 0; i < ae->e_count; i++)
        {
            uint64_t v = base + strings_off + ae->e_off[i];
            copy_bytes(buf + off, (const uint8_t*)&v, 8);
            off += 8;
        }
        off += 8;

        // --- auxv, AT_NULL-terminated ---
        struct { uint64_t type; uint64_t val; } entry;
        if (lr->phdr_vaddr)
        {
            entry.type = AT_PHDR;  entry.val = lr->phdr_vaddr;
            copy_bytes(buf + off, (const uint8_t*)&entry, 16);
            off += 16;
        }
        entry.type = AT_PHENT;  entry.val = lr->phdr_entsize;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;
        entry.type = AT_PHNUM;  entry.val = lr->phdr_num;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;
        entry.type = AT_PAGESZ; entry.val = PAGE_SIZE_4K;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;
        entry.type = AT_ENTRY;  entry.val = lr->entry;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;
        entry.type = AT_RANDOM; entry.val = base + random_off;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;
        entry.type = AT_NULL;   entry.val = 0;
        copy_bytes(buf + off, (const uint8_t*)&entry, 16); off += 16;

        // --- random bytes and the strings ---
        copy_bytes(buf + random_off, random_bytes, sizeof(random_bytes));
        copy_bytes(buf + strings_off, (const uint8_t*)ae->data, strings_size);

        poke_user(base, buf, total);
        kfree(buf);

        *out_rsp = base;
        return 0;
    }

    // Build a complete address space for `path` (resolved against `cwd`):
    // ELF segments, stack with the SysV argv/envp/auxv block. The active
    // address space is unchanged on return. Returns 0 or -errno.
    static sint64_t load_program(const char* path, vnode* cwd,
                                 const ArgEnv* ae, Image* out)
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

        uint64_t rsp = 0;
        if (ok)
        {
            sint64_t rc = build_initial_stack(ae, &lr, &rsp);
            if (rc < 0)
            {
                err = (int)-rc;
                ok = false;
            }
        }

        paging::switch_address_space(prev);
        vfs::unref(v);

        if (!ok)
        {
            paging::destroy_address_space(as);
            return -(sint64_t)err;
        }

        out->cr3       = as;
        out->entry     = lr.entry;
        out->image_end = lr.image_end;
        out->rsp       = rsp;
        return 0;
    }

    static void adopt_image(Process* p, const Image* img, const char* path)
    {
        p->cr3         = img->cr3;
        p->brk_start   = img->image_end;
        p->brk         = img->image_end;
        p->mmap_cursor = USER_MMAP_BASE;
        copy_name(p->name, path);
        initial_context(&p->ctx, img->entry, img->rsp);
        copy_bytes(p->fpu, fpu_template, sizeof(p->fpu));
    }

    // -----------------------------------------------------------------------
    // Scheduler
    // -----------------------------------------------------------------------

    // Post SIGCHLD to p's parent (defined with the rest of the signal
    // machinery, below).
    static void notify_parent(Process* p);

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

        // POSIX: descriptors close and the cwd is released when the process
        // exits, not when the parent reaps the zombie.
        filesys::fdtable_close_all(&p->fds);
        if (p->cwd)
        {
            vfs::unref(p->cwd);
            p->cwd = nullptr;
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
            notify_parent(p);
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
            // A stop or a resume the parent asked to hear about is an event
            // in its own right, even though the child is still there.
            if ((q->stop_pending && (p->wait_opts & WUNTRACED)) ||
                (q->cont_pending && (p->wait_opts & WCONTINUED)))
                return true;
            has_child = true;
        }
        return !has_child;     // nothing left to wait for: let waitpid fail
    }

    static bool wake_ready(Process* p)
    {
        // A deliverable signal ends any wait: this is what makes a blocking
        // read interruptible, and what lets a handler run at all while the
        // process sits in read() or wait().
        if (sig::next_deliverable(&p->sig))
            return true;

        switch (p->wait)
        {
            case Wait::Key:    return tty::readable();
            case Wait::Child:  return child_event(p);
            case Wait::Sleep:  return pit::ticks() >= p->wake_tick;
            case Wait::Signal: return false;    // pause/sigsuspend: only a signal
            default:           return true;
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

    // Something that could still be scheduled, as opposed to merely
    // existing: a session of nothing but stopped processes is stalled.
    static bool any_runnable()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (table[i].state == State::Runnable || table[i].state == State::Blocked)
                return true;
        return false;
    }

    static bool any_live()
    {
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            if (alive(&table[i]))
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
        // Esc ends the session: every process dies as if by SIGINT, which is
        // what a terminal interrupt would deliver in Linux.
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (alive(p))
                terminate(p, signal_status(SIGINT));
        }
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
    // restores the saved context wholesale. That is why signals fit the
    // existing "rewind RIP and restart" model without the scheduler having
    // to learn how to sleep on a kernel stack.

    const sigset_t STOP_SIGNALS = SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) |
                                  SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU);

    static inline int stop_code(int n) { return 0x7F | ((n & 0xFF) << 8); }

    static void notify_parent(Process* p)
    {
        if (p->ppid == 0)
            return;                     // the session is the parent
        Process* parent = find_live(p->ppid);
        if (parent)
            sig::post(&parent->sig, SIGCHLD);
    }

    static void post_signal(Process* p, int n)
    {
        if (!sig::valid(n) || !alive(p))
            return;

        // SIGCONT resumes before any question of handlers: a stopped
        // process cannot run its own handler until it is running again.
        if (n == SIGCONT)
        {
            p->sig.pending &= ~STOP_SIGNALS;
            if (p->state == State::Stopped)
            {
                p->state = State::Runnable;
                p->wait  = Wait::None;
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
            if (!alive(p) || p->pgid != pgid)
                continue;
            count++;
            if (n)
                post_signal(p, n);
        }
        return count;
    }

    // The tty turns ^C, ^\\ and ^Z into a signal for the foreground group.
    // The conversion happens here rather than in the keyboard IRQ: posting
    // touches the process table, and the IRQ can land anywhere.
    static void tty_signals()
    {
        if (tty::take_kill())
            kill_requested = true;

        int n;
        while ((n = tty::take_signal()) != 0)
            signal_group(tty::fg_pgrp(), n);
    }

    static void stop_process(Process* p, int n, user_regs* regs, iret_frame* iret)
    {
        if (p == current)
            save_context(p, regs, iret);

        p->state = State::Stopped;
        p->wait  = Wait::None;
        p->stop_status  = stop_code(n);
        p->stop_pending = true;
        notify_parent(p);
    }

    // Apply every pending signal whose action needs no user code. Returns
    // true when `current` can no longer continue and the caller has to
    // reschedule.
    static bool service_signals(user_regs* regs, iret_frame* iret)
    {
        bool switch_away = false;

        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];

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
                        stop_process(p, n, regs, iret);
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

    // Build the handler frame on the user stack and point the trap frame at
    // the handler. False when the stack is unusable.
    static bool push_signal_frame(Process* p, int n, user_regs* regs,
                                  iret_frame* iret)
    {
        const k_sigaction act = p->sig.act[n];
        if (!act.restorer)
            return false;               // the SDK always supplies one

        // EINTR or SA_RESTART. block() rewound RIP over the `int 0x80`, so
        // the context about to be saved would re-execute the syscall after
        // the handler returns - which is exactly SA_RESTART. Without it the
        // interrupted call has to fail instead, so step back over the
        // rewind and plant the error before the context is captured.
        if (p->rewound)
        {
            if (!(act.flags & SA_RESTART))
            {
                iret->rip += INT80_LENGTH;
                regs->rax = SYSCALL_ERR(EINTR);
            }
            p->rewound = false;
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

    // Choose what runs next and load it into the trap frame. The caller has
    // already saved the current process (if it is still alive). Never returns
    // to the caller when the session is over.
    static void reschedule(user_regs* regs, iret_frame* iret)
    {
        bool stall_reported = false;

        for (;;)
        {
            if (kill_requested)
            {
                screen::printf("\n[interrupted]\n");
                kill_session();
                end_session();
            }

            tty_signals();
            service_signals(regs, iret);

            if (!any_live())
                end_session();

            Process* next = pick_next();
            if (next)
            {
                slice_ticks = 0;
                load_context(next, regs, iret);
                // The address space is live now, so the frame can be
                // written. If that fails the process is gone: pick again.
                if (deliver_signals(next, regs, iret))
                    continue;
                return;
            }

            if (!stall_reported && !any_runnable())
            {
                // Every process is stopped and nothing in the session can
                // send SIGCONT, because the console is not running while a
                // session is. Esc is the way out.
                stall_reported = true;
                screen::printf("\n[stopped - press Esc to end the session]\n");
            }

            // Everyone is waiting (for a key, a timer, a child, a signal).
            // Idle until an interrupt changes that. Interrupts arriving here
            // come from ring 0, so they never re-enter the scheduler.
            asm volatile("sti; hlt");
        }
    }

    // Everything that has to happen on the way back to ring 3 when the
    // scheduler was not otherwise involved.
    static void return_to_user(user_regs* regs, iret_frame* iret)
    {
        tty_signals();

        if (!current)
        {
            reschedule(regs, iret);
            return;
        }

        if (service_signals(regs, iret) || !current ||
            current->state != State::Runnable)
        {
            reschedule(regs, iret);
            return;
        }

        if (deliver_signals(current, regs, iret))
            reschedule(regs, iret);
    }

    // Put the current process to sleep. With `restart`, the syscall is
    // re-executed from scratch when the process wakes up; otherwise it
    // returns whatever the caller left in regs->rax.
    static void block(Wait why, bool restart, user_regs* regs, iret_frame* iret)
    {
        if (restart)
            iret->rip -= INT80_LENGTH;
        current->rewound = restart;

        current->state = State::Blocked;
        current->wait = why;
        save_context(current, regs, iret);
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
        }
        return filesys::any_open_on(m);
    }

    void init()
    {
        memory::memset((uint8_t*)table, 0x00, sizeof(table));
        vfs::set_busy_hook(mount_in_use);

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

    // stdin/stdout/stderr: one /dev/tty opened once and dup'ed onto fds
    // 0, 1 and 2 of the session's root process (children inherit through
    // the fd-table fork).
    static void open_std_fds(Process* p)
    {
        vnode* tty_vn = nullptr;
        if (vfs::lookup("/dev/tty", nullptr, &tty_vn, false) != 0)
            return;             // no devfs yet: syscalls will fail EBADF

        file* f = filesys::file_open(tty_vn, O_RDWR);   // takes the ref
        if (!f)
            return;

        // Three slots sharing one open file description. file_open handed us
        // one reference; slots 1 and 2 each take another. On any failure the
        // references taken so far have to go back, or the description and its
        // vnode are pinned for the lifetime of the kernel.
        sint32_t fd = -1;
        filesys::file_get(f);
        if (filesys::fdtable_alloc(&p->fds, f, false, &fd) != 0 || fd != 0)
        {
            // Either the slot took a reference (fd != 0) or alloc already
            // gave one back (-EMFILE); either way the one file_open handed
            // us is still outstanding.
            filesys::file_put(f);
            return;
        }
        filesys::file_get(f);
        if (filesys::fdtable_alloc(&p->fds, f, false, &fd) != 0 || fd != 1)
        {
            filesys::file_put(f);
            return;
        }
        if (filesys::fdtable_alloc(&p->fds, f, false, &fd) != 0)   // fd 2
            return;                     // alloc already released it
    }

    // Load `path` with the collected argv/envp and allocate a runnable
    // process for it. ppid is left at 0; state is Runnable.
    static Process* launch(const char* path, const ArgEnv* ae)
    {
        Image img;
        if (load_program(path, vfs::cwd(), ae, &img) < 0)
            return nullptr;

        Process* p = alloc_process();
        if (!p)
        {
            paging::destroy_address_space(img.cr3);
            return nullptr;
        }
        adopt_image(p, &img, path);
        p->state = State::Runnable;
        return p;
    }

    // Called by run() after launch(): the root process gets its std fds.

    // Build the default environment a console-launched process starts with.
    // 0 or -errno.
    static int push_default_env(ArgEnv* ae)
    {
        int rc = ae->push_kstr(true, "PATH=/bin");
        if (rc) return rc;
        rc = ae->push_kstr(true, "HOME=/");
        if (rc) return rc;
        rc = ae->push_kstr(true, "TERM=dumb");
        if (rc) return rc;

        // PWD=<console cwd> (the system cwd; per-process cwd takes over in
        // 3.5 when the root process inherits it at launch).
        char pwdbuf[PATH_MAX + 8];
        const char prefix[] = "PWD=";
        copy_bytes((uint8_t*)pwdbuf, (const uint8_t*)prefix, 4);
        sint64_t prc = vfs::cwd_path(pwdbuf + 4, PATH_MAX);
        if (prc != 0)
            return (int)prc;
        return ae->push_kstr(true, pwdbuf);
    }

    bool run(const char* path, int argc, const char* const* argv, int* exit_status)
    {
        if (session_active)
            return false;

        ArgEnv ae;
        if (!ae.init())
            return false;

        int rc = 0;
        if (argc <= 0)
            rc = ae.push_kstr(false, path);
        else
            for (int i = 0; i < argc && rc == 0; i++)
                rc = ae.push_kstr(false, argv[i]);

        if (rc == 0)
            rc = push_default_env(&ae);

        Process* p = rc == 0 ? launch(path, &ae) : nullptr;
        ae.destroy();

        if (!p)
            return false;

        p->ppid = 0;
        open_std_fds(p);

        root_pid       = p->pid;
        root_status    = 0;
        kill_requested = false;
        session_active = true;
        slice_ticks    = 0;
        last_slot      = (uint32_t)(p - table);

        tty::reset();
        // The root process starts in the foreground: ^C goes to its group,
        // and it is the one allowed to read the keyboard.
        tty::set_fg_pgrp(p->pgid);
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

        tss::set_kernel_stack(kstack_top(p));

        current = p;
        paging::switch_address_space(p->cr3);
        fpu_restore(p->fpu);

        cpu_context* frame = (cpu_context*)(kstack_top(p) - sizeof(cpu_context));
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
        free_kernel_stacks();   // we are back on the console stack

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

        if (irq == IRQ0_TIMER && ++slice_ticks >= TIME_SLICE_TICKS)
        {
            save_context(current, regs, iret);
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

    // Called at the top of every syscall: the saved context is about to be
    // superseded by a real execution, so the rewind block() recorded is no
    // longer outstanding.
    void syscall_enter()
    {
        if (current)
            current->rewound = false;
    }

    void syscall_return(user_regs* regs, iret_frame* iret)
    {
        if (kill_requested)
        {
            reschedule(regs, iret);
            return;
        }
        return_to_user(regs, iret);
    }

    // -----------------------------------------------------------------------
    // Hooks for sys_fs.cpp (file-descriptor syscalls)
    // -----------------------------------------------------------------------

    fd_table* cur_fds()
    {
        return current ? &current->fds : nullptr;
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
        if (!current)
            return true;                // no session: the console owns the tty
        pid_t fg = tty::fg_pgrp();
        return fg == 0 || fg == current->pgid;
    }

    pid_t cur_pgrp()
    {
        return current ? current->pgid : 0;
    }

    int signal_pgrp(pid_t pgid, int sig)
    {
        return signal_group(pgid, sig);
    }

    void block_on_input(user_regs* regs, iret_frame* iret)
    {
        // Rewind RIP over `int 0x80` and mark Wait::Key: on wake the syscall
        // re-executes with its registers intact. No fd side effect happened
        // before this point, so the restart is safe.
        block(Wait::Key, true, regs, iret);
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

    // No threads yet: exit_group terminates exactly the calling process.
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
        save_context(current, regs, iret);
        reschedule(regs, iret);
    }

    // nanosleep(req, rem): req/rem are user `struct timespec`. With no
    // signals rem is always zero; it must be written before blocking - the
    // wake path resumes the user process from the saved context, kernel C
    // code after block() does not re-run on a normal wake.
    void sys_nanosleep(user_regs* regs, iret_frame* iret)
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

        if (rem_ptr)
        {
            timespec rem = { 0, 0 };
            if (!uaccess::copy_to_user(rem_ptr, &rem, sizeof(rem)))
            {
                regs->rax = SYSCALL_ERR(EFAULT);
                return;
            }
        }

        uint64_t ms = (uint64_t)req.tv_sec * 1000ULL + (uint64_t)(req.tv_nsec / 1000000);
        if (ms == 0)
            ms = 1;             // never a busy spin

        uint32_t hz = pit::real_frequency();
        if (!hz)
            hz = pit::frequency();
        uint64_t ticks = (ms * hz + 999) / 1000;
        current->wake_tick = pit::ticks() + (ticks ? ticks : 1);

        regs->rax = 0;
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
        child->pgid        = current->pgid;     // same job as its parent
        child->brk_start   = current->brk_start;
        child->brk         = current->brk;
        child->mmap_cursor = current->mmap_cursor;

        // POSIX: the child shares the parent's open file descriptions (same
        // offsets) and inherits its cwd and umask. alloc_process gave the
        // child the system cwd; replace it with the parent's.
        filesys::fdtable_fork(&child->fds, &current->fds);
        child->umask = current->umask;
        if (child->cwd)
            vfs::unref(child->cwd);
        child->cwd = current->cwd;
        if (child->cwd)
            vfs::ref(child->cwd);

        // Handlers and the blocked mask carry over; pending signals do not
        // (POSIX: the child starts with an empty pending set).
        sig::inherit(&child->sig, &current->sig);
        child->rewound    = false;
        child->mask_saved = false;

        // The child resumes from the same instruction with rax = 0.
        child->ctx.regs = *regs;
        child->ctx.iret = *iret;
        child->ctx.regs.rax = 0;
        fpu_save(child->fpu);

        child->state = State::Runnable;
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

        adopt_image(current, &img, path);   // copies the name it needs
        kfree(path);

        // execve keeps the fd table except CLOEXEC slots, and keeps cwd and
        // umask (POSIX). adopt_image reset only the address-space fields.
        filesys::fdtable_cloexec(&current->fds);

        // Every handler address belonged to the image that has just been
        // replaced; ignored signals and the blocked mask survive.
        sig::reset_on_exec(&current->sig);
        current->rewound    = false;
        current->mask_saved = false;

        fpu_restore(current->fpu);
        *regs = current->ctx.regs;
        *iret = current->ctx.iret;
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

        current->wait_pid  = pid;
        current->wait_opts = options;
        block(Wait::Child, true, regs, iret);
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
            if (t)
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
            // Every process we may signal, which here means the session
            // apart from the caller.
            for (uint32_t i = 0; i < MAX_PROCESSES; i++)
            {
                Process* p = &table[i];
                if (!alive(p) || p == current)
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

        current->rewound    = false;
        current->mask_saved = false;
    }

    // pause(): wait for any signal that runs a handler.
    void sys_pause(user_regs* regs, iret_frame* iret)
    {
        regs->rax = SYSCALL_ERR(EINTR);     // the only way pause returns
        block(Wait::Signal, false, regs, iret);
    }

    // rt_sigsuspend(mask, sigsetsize): swap the mask, wait, and let the
    // sigframe put the old one back - that is what makes it atomic.
    void sys_rt_sigsuspend(user_regs* regs, iret_frame* iret)
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

        regs->rax = SYSCALL_ERR(EINTR);
        block(Wait::Signal, false, regs, iret);
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

    // There is one session (the console runs one program at a time), so
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
        if (!tty::pop_key(&e))
        {
            block(Wait::Key, true, regs, iret);
            return;
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

        regs->rax = 0;
    }
} // namespace process
