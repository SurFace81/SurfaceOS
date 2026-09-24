// Kernel tasks: stack switching. See task.h.

#include "../../include/cpu/task.h"

extern "C" void task_switch(uint64_t* save_rsp, uint64_t new_rsp);
extern "C" void task_kernel_start();
extern "C" void task_user_start();

namespace task
{
    namespace
    {
        Task* running = nullptr;

        // What task_switch pops, lowest address first.
        struct switch_frame
        {
            uint64_t r15, r14, r13, r12, rbx, rbp;
            uint64_t rflags;
            uint64_t ret;
        };

        const uint64_t RFLAGS_KERNEL = 0x2;     // reserved bit 1, IF clear

        void fill(switch_frame* f, uint64_t ret, uint64_t rbx, uint64_t r12)
        {
            f->r15 = f->r14 = f->r13 = f->rbp = 0;
            f->rbx    = rbx;
            f->r12    = r12;
            f->rflags = RFLAGS_KERNEL;
            f->ret    = ret;
        }
    }

    void init(Task* boot, const char* name)
    {
        boot->rsp = 0;
        boot->name = name;
        running = boot;
    }

    Task* current()
    {
        return running;
    }

    void switch_to(Task* next)
    {
        Task* prev = running;
        if (prev == next)
            return;
        running = next;
        task_switch(&prev->rsp, next->rsp);
    }

    void prepare_kernel(Task* t, const char* name, uint64_t stack_top,
                        void (*entry)(void*), void* arg)
    {
        // After the pops and the `ret`, rsp is stack_top: aligned for the
        // `call` in task_kernel_start.
        switch_frame* f = (switch_frame*)(stack_top - sizeof(switch_frame));
        fill(f, (uint64_t)task_kernel_start, (uint64_t)arg, (uint64_t)entry);

        t->rsp  = (uint64_t)f;
        t->name = name;
    }

    void prepare_user(Task* t, const char* name, uint64_t stack_top,
                      const cpu_context* ctx)
    {
        cpu_context* c = (cpu_context*)(stack_top - sizeof(cpu_context));
        *c = *ctx;

        // iretq takes IF from the user RFLAGS.
        switch_frame* f = (switch_frame*)((uint64_t)c - sizeof(switch_frame));
        fill(f, (uint64_t)task_user_start, 0, 0);

        t->rsp  = (uint64_t)f;
        t->name = name;
    }
}
