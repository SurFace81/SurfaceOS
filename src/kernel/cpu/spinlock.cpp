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
    // Owner and depth change with this CPU's interrupts off: an interrupt
    // between taking the lock and setting the depth would see the lock as
    // its own at depth 0 and let it go on the way out.
    static inline uint64_t irq_off()
    {
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        return flags;
    }

    static inline void irq_restore(uint64_t flags)
    {
        asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
    }

    void enter()
    {
        for (;;)
        {
            uint64_t flags = irq_off();
            Cpu* c = cpu::current();
            uint32_t me = c->index + 1;
            uint32_t free = 0;
            if (bkl_owner == me)
            {
                c->bkl_depth++;
                irq_restore(flags);
                return;
            }
            if (__atomic_compare_exchange_n(&bkl_owner, &free, me, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            {
                c->bkl_depth = 1;
                irq_restore(flags);
                return;
            }
            irq_restore(flags);         // interrupts may come in while waiting
            while (bkl_owner)
                asm volatile("pause");
        }
    }

    bool try_enter()
    {
        uint64_t flags = irq_off();
        Cpu* c = cpu::current();
        uint32_t me = c->index + 1;
        uint32_t free = 0;
        bool got = true;
        if (bkl_owner == me)
            c->bkl_depth++;
        else if (__atomic_compare_exchange_n(&bkl_owner, &free, me, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            c->bkl_depth = 1;
        else
            got = false;
        irq_restore(flags);
        return got;
    }

    void leave()
    {
        uint64_t flags = irq_off();
        Cpu* c = cpu::current();
        if (bkl_owner != c->index + 1 || c->bkl_depth == 0)
            uart::printf("bkl: cpu %u leaves a lock it does not hold\n", c->index);
        else if (--c->bkl_depth == 0)
            __atomic_store_n(&bkl_owner, 0, __ATOMIC_RELEASE);
        irq_restore(flags);
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
