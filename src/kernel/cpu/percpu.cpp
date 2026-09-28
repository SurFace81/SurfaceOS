// Per-CPU data, TSS and GDT. See percpu.h.

#include "../../include/cpu/percpu.h"
#include "../../include/mm/memory.h"

namespace
{
    const uint32_t MSR_GS_BASE        = 0xC0000101;
    const uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

    Cpu boot_cpu;

    // The boot CPU's IST stacks: #DF, NMI, #MC.
    uint8_t boot_ist[3 * IST_STACK_SIZE] __attribute__((aligned(16)));

    inline void wrmsr(uint32_t msr, uint64_t v)
    {
        asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    }

    // The 16-byte GDT system descriptor of a 64-bit TSS.
    void install_tss(GDT_t* gdt, tss_t* tss)
    {
        uint64_t base = (uint64_t)tss;
        uint32_t limit = sizeof(tss_t) - 1;

        uint64_t low = 0;
        low |= (uint64_t)(limit & 0xFFFF);
        low |= (uint64_t)((base & 0xFFFF) << 16);
        low |= (uint64_t)(((base >> 16) & 0xFF) << 32);
        low |= (uint64_t)0x89 << 40;                  // present, available 64-bit TSS
        low |= (uint64_t)((limit >> 16) & 0xF) << 48;
        low |= (uint64_t)((base >> 24) & 0xFF) << 56;

        gdt->TssLow  = low;
        gdt->TssHigh = base >> 32;
    }
}

namespace cpu
{
    void setup(Cpu* c, GDT_t* gdt, uint8_t* ist_stacks)
    {
        c->self = c;
        c->gdt  = gdt;
        if (gdt != &DefaultGDT)
            memory::memcpy((uint8_t*)gdt, (const uint8_t*)&DefaultGDT, sizeof(GDT_t));

        memory::memset((uint8_t*)&c->tss, 0x00, sizeof(c->tss));
        c->tss.ist1 = (uint64_t)ist_stacks + 1 * IST_STACK_SIZE;    // IST_DOUBLE_FAULT
        c->tss.ist2 = (uint64_t)ist_stacks + 2 * IST_STACK_SIZE;    // IST_NMI
        c->tss.ist3 = (uint64_t)ist_stacks + 3 * IST_STACK_SIZE;    // IST_MACHINE_CHECK
        // iomap_base past the segment limit means "no I/O permission bitmap",
        // so every I/O port access from ring 3 raises #GP.
        c->tss.iomap_base = sizeof(tss_t);
        install_tss(gdt, &c->tss);

        gdt_ptr_t ptr;
        ptr.limit = sizeof(GDT_t) - 1;
        ptr.base  = (uint64_t)gdt;
        LoadGDT(&ptr);                  // reloads the segments: GS base goes to 0
        asm volatile("ltr %0" :: "r"((uint16_t)TSS_SEGMENT));

        // Kernel code runs with this CPU's Cpu in the GS base; ring 3 starts
        // with a GS base of 0, swapped in on the way out.
        wrmsr(MSR_GS_BASE, (uint64_t)c);
        wrmsr(MSR_KERNEL_GS_BASE, 0);
    }

    void init_boot_cpu()
    {
        boot_cpu.index = 0;
        setup(&boot_cpu, &DefaultGDT, boot_ist);
    }

    void set_kernel_stack(uint64_t rsp0)
    {
        Cpu* c = current();
        c->tss.rsp0   = rsp0;
        c->kernel_rsp = rsp0;
    }
}
