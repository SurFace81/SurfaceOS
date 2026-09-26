// Starting the other CPUs. See smp.h.

#include "../../include/cpu/smp.h"
#include "../../include/cpu/percpu.h"
#include "../../include/cpu/apic.h"
#include "../../include/cpu/idt.h"
#include "../../include/cpu/features.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/process.h"
#include "../../include/acpi/acpi.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"

extern "C" const uint8_t ap_trampoline_start[];
extern "C" const uint8_t ap_trampoline_end[];

namespace
{
    // ap_trampoline.asm's parameter block, from the start of its page.
    const uint64_t TRAMPOLINE_PARAMS = 0xF00;

    struct trampoline_params
    {
        uint64_t cr3;
        uint64_t stack;
        uint64_t cpu;
        uint64_t entry;
    };

    // What a started CPU gets, in one run of frames: its Cpu, its GDT, the
    // three IST stacks, and the kernel stack it starts (and idles) on.
    const uint64_t BLOCK_CPU    = 0;
    const uint64_t BLOCK_GDT    = 1;
    const uint64_t BLOCK_IST    = 2;
    const uint64_t IST_PAGES    = 3 * IST_STACK_SIZE / PAGE_SIZE_4K;
    const uint64_t BLOCK_STACK  = BLOCK_IST + IST_PAGES;
    const uint64_t STACK_PAGES  = 4;
    const uint64_t BLOCK_PAGES  = BLOCK_STACK + STACK_PAGES;

    uint32_t cpus_running = 1;

    uint8_t* block_page(Cpu* c, uint64_t page)
    {
        return (uint8_t*)c + page * PAGE_SIZE_4K;
    }

    // Busy-wait `ticks` timer ticks (about a millisecond each).
    void wait_ticks(uint64_t ticks)
    {
        uint64_t t = pit::ticks();
        while (pit::ticks() - t < ticks)
            asm volatile("pause");
    }

    bool wait_online(Cpu* c, uint64_t ticks)
    {
        uint64_t t = pit::ticks();
        while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
        {
            if (pit::ticks() - t >= ticks)
                return false;
            asm volatile("pause");
        }
        return true;
    }
}

// A started CPU arrives here from the trampoline, on its own stack.
extern "C" __attribute__((noreturn)) void ap_main(Cpu* c)
{
    cpu::setup(c, (GDT_t*)block_page(c, BLOCK_GDT), block_page(c, BLOCK_IST));
    idt::load();
    cpu::init_features();
    sfcall::init_cpu();
    c->apic_id = apic::init_cpu();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);

    // Its own tick, and on to running threads (its idle loop).
    apic::start_timer_cpu();
    process::run_cpu();
}

namespace smp
{
    void start(uint64_t trampoline)
    {
        const acpi::madt_info* m = acpi::madt();
        uint64_t len = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
        if (!trampoline || !m->present || m->cpu_count < 2 || len > PAGE_SIZE_4K)
        {
            uart::printf("smp: one CPU (%s)\n", !trampoline ? "no trampoline page"
                                                : m->cpu_count < 2 ? "no other CPU listed"
                                                : "no MADT");
            return;
        }

        // The trampoline, and its page identity-mapped: the switch to long
        // mode goes on at its low addresses.
        memory::memcpy((uint8_t*)phys_to_virt(trampoline), ap_trampoline_start, len);
        if (!paging::map_page(trampoline, trampoline, PAGE_WRITE))
        {
            uart::printf("smp: cannot map the trampoline page\n");
            return;
        }
        trampoline_params* params =
            (trampoline_params*)((uint8_t*)phys_to_virt(trampoline) + TRAMPOLINE_PARAMS);

        uint32_t me = apic::id();
        uint32_t index = 1;
        for (uint32_t i = 0; i < m->cpu_count; i++)
        {
            const acpi::cpu_info& info = m->cpus[i];
            if (!info.enabled || info.apic_id == me)
                continue;

            uint64_t frames = pmm::alloc_frames(BLOCK_PAGES);
            if (!frames)
            {
                uart::printf("smp: no memory for more CPUs\n");
                break;
            }
            Cpu* c = (Cpu*)phys_to_virt(frames);
            memory::memset((uint8_t*)c, 0x00, PAGE_SIZE_4K);
            c->index   = index;
            c->apic_id = info.apic_id;

            params->cr3   = paging::kernel_pml4();
            params->stack = (uint64_t)block_page(c, BLOCK_PAGES);
            params->cpu   = (uint64_t)c;
            params->entry = (uint64_t)&ap_main;

            // INIT, 10 ms, a startup IPI - and a second one if the CPU has
            // not answered within a millisecond or so.
            apic::send_init(info.apic_id);
            wait_ticks(10);
            apic::send_startup(info.apic_id, trampoline);
            bool up = wait_online(c, 2);
            if (!up)
            {
                apic::send_startup(info.apic_id, trampoline);
                up = wait_online(c, 200);
            }
            if (!up)
            {
                uart::printf("smp: cpu with APIC id %u did not start\n", info.apic_id);
                pmm::free_frames(frames, BLOCK_PAGES);
                continue;
            }
            index++;
            cpus_running++;
        }

        paging::unmap_page(trampoline);
        uart::printf("smp: %u CPU(s) running\n", cpus_running);
    }

    uint32_t running()
    {
        return cpus_running;
    }
}
