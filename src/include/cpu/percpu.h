#ifndef PERCPU_H
#define PERCPU_H

#include "types.h"
#include "tss.h"
#include "gdt.h"

// What each CPU keeps for itself: found through the GS base, which points
// at the CPU's own Cpu while it runs kernel code.
//
// A program can load GS itself and so change the GS base, so the kernel
// keeps its own: every entry from ring 3 (interrupts, exceptions, int 0x80,
// the syscall instruction) swaps the program's GS base out with `swapgs`,
// and every return to ring 3 swaps it back (interrupts.asm, task.asm).
//
// Each CPU has its own TSS (the kernel stack a trap from ring 3 lands on,
// and the IST stacks for #DF, NMI and #MC), and therefore its own GDT: the
// TSS descriptor is marked busy by the CPU that loads it.

struct Cpu
{
    Cpu*     self;          // gs:0  - cpu::current() reads it
    uint64_t kernel_rsp;    // gs:8  - the running thread's kernel stack (= tss.rsp0),
                            //         for the syscall entry
    uint64_t user_rsp;      // gs:16 - the syscall entry parks the user rsp here
    uint32_t index;         // 0 for the boot CPU
    uint32_t apic_id;
    uint32_t bkl_depth;     // how often it holds the big kernel lock (spinlock.h)
    GDT_t*   gdt;
    tss_t    tss;
};

// Offsets the assembly entry code uses.
#define CPU_SELF        0
#define CPU_KERNEL_RSP  8
#define CPU_USER_RSP    16

namespace cpu
{
    // The boot CPU's: its GDT (DefaultGDT, already loaded by gdt::init),
    // TSS and GS base.
    void init_boot_cpu();

    // Set up the CPU this runs on with `c`: load its GDT (a copy of the
    // default one) with its TSS, whose IST stacks are `ist_stacks` (three
    // stacks of IST_STACK_SIZE bytes, top down), and point the GS base at
    // it.
    void setup(Cpu* c, GDT_t* gdt, uint8_t* ist_stacks);

    // The CPU this runs on.
    inline Cpu* current()
    {
        Cpu* c;
        asm volatile("mov %%gs:0, %0" : "=r"(c));
        return c;
    }

    // The kernel stack a trap from ring 3 lands on, on this CPU.
    void set_kernel_stack(uint64_t rsp0);
}

#define IST_STACK_SIZE (16 * 1024)

#endif // PERCPU_H
