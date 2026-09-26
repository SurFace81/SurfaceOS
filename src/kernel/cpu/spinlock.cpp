// Spin locks and the big kernel lock. See spinlock.h.

#include "../../include/cpu/spinlock.h"
#include "../../include/cpu/percpu.h"
#include "../../include/drivers/uart.h"

namespace
{
    // Which CPU holds the big kernel lock: its index + 1, 0 when free.
    volatile uint32_t bkl_owner = 0;
}

namespace spin
{
    void lock(spinlock* l)
    {
        while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
            while (l->locked)
                asm volatile("pause");
    }

    void unlock(spinlock* l)
    {
        __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    }

    uint64_t lock_irq(spinlock* l)
    {
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        lock(l);
        return flags;
    }

    void unlock_irq(spinlock* l, uint64_t flags)
    {
        unlock(l);
        asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
    }
}

namespace bkl
{
    void enter()
    {
        Cpu* c = cpu::current();
        uint32_t me = c->index + 1;
        if (bkl_owner == me)
        {
            c->bkl_depth++;
            return;
        }
        uint32_t free = 0;
        while (!__atomic_compare_exchange_n(&bkl_owner, &free, me, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            free = 0;
            asm volatile("pause");
        }
        c->bkl_depth = 1;
    }

    void leave()
    {
        Cpu* c = cpu::current();
        if (bkl_owner != c->index + 1 || c->bkl_depth == 0)
        {
            uart::printf("bkl: cpu %u leaves a lock it does not hold\n", c->index);
            return;
        }
        if (--c->bkl_depth == 0)
            __atomic_store_n(&bkl_owner, 0, __ATOMIC_RELEASE);
    }

    void check_switch()
    {
        Cpu* c = cpu::current();
        if (bkl_owner != c->index + 1 || c->bkl_depth != 1)
            uart::printf("bkl: cpu %u switches threads at depth %u (owner %u)\n",
                         c->index, c->bkl_depth, bkl_owner);
    }
}

extern "C" void bkl_leave_to_user()
{
    bkl::leave();
}
