// Processes: table, ELF program loading, the scheduler, and the process and
// memory syscalls. See process.h for the scheduling model.
//
// User memory is W^X throughout: code is read+execute, everything writable
// (data, heap, stack) is NX. mmap/mprotect are the one sanctioned way to get
// RWX memory, which a JIT or tcc -run needs.

#include "../../include/cpu/process.h"
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
        // Kernel stack for traps taken while this process runs (TSS RSP0).
        // Owned by the table *slot*, not by the process: terminate() can
        // free a process while running on this very stack, so it is released
        // only at session teardown, from the console stack.
        uint64_t    kstack;     // base address, 0 when the slot has none

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
                pmm::free_frames(table[i].kstack, KERNEL_STACK_FRAMES);
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
                p->kstack = pmm::alloc_frames(KERNEL_STACK_FRAMES);
                if (!p->kstack)
                    return nullptr;     // slot stays Unused
            }
            p->pid = next_pid++;
            if (next_pid <= 0)
                next_pid = 1;

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
    // identity map, so the stack can stay mapped as it is. Returns the
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
            case Wait::Key:   return tty::readable();
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
        // Esc ends the session: every process dies as if by SIGINT, which is
        // what a terminal interrupt would deliver in Linux.
        for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        {
            Process* p = &table[i];
            if (p->state == State::Runnable || p->state == State::Blocked)
                terminate(p, signal_status(SIGINT));
        }
    }

    // Choose what runs next and load it into the trap frame. The caller has
    // already saved the current process (if it is still alive). Never returns
    // to the caller when the session is over.
    static void reschedule(user_regs* regs, iret_frame* iret)
    {
        for (;;)
        {
            if (kill_requested || tty::intr_pressed())
            {
                screen::printf("\n[interrupted]\n");
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

        if (irq != IRQ0_TIMER)
            return;

        if (++slice_ticks < TIME_SLICE_TICKS)
            return;

        save_context(current, regs, iret);
        reschedule(regs, iret);
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
        screen::printf("\n[pid %u %s terminated: CPU exception %u]\n",
                       (uint32_t)current->pid, current->name, (uint32_t)vector);

        terminate(current, signal_status(vector_to_signal(vector)));
        reschedule(regs, iret);
    }

    void syscall_return(user_regs* regs, iret_frame* iret)
    {
        if (kill_requested)
            reschedule(regs, iret);
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
            terminate(current, signal_status(SIGKILL));
            reschedule(regs, iret);
            return;
        }

        terminate(target, signal_status(SIGKILL));
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
