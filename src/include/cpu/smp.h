#ifndef SMP_H
#define SMP_H

#include "types.h"

// The other CPUs. The boot CPU starts each one the MADT lists (INIT, then
// startup IPIs into ap_trampoline.asm at the page the loader claimed); a
// started CPU sets up its own GDT, TSS, GS base, IDT, features, syscall
// MSRs, local APIC and timer, says it is online and goes on to run threads
// (process::run_cpu).

namespace smp
{
    // Start the other CPUs; needs the APIC and a ticking timer.
    // `trampoline` is BOOT_HEADER.ApTrampolineAddress (0: stay on one CPU).
    void start(uint64_t trampoline);

    // CPUs running, the boot CPU included; their indexes are 0..running()-1.
    uint32_t running();

    // After a user address space lost pages or permissions: make every
    // other CPU that has it loaded drop its TLB entries, and wait until
    // they have. Call with the big kernel lock held (it keeps anyone from
    // switching to that space meanwhile).
    void flush_tlb(uint64_t cr3);

    // On CPU 1's tick: the boot CPU counts the ticks and draws the screen.
    // If the count stands still for 3 s, it is stuck with interrupts off;
    // an NMI makes it report where (idt.cpp) instead of a frozen screen.
    void watch_boot_cpu();
}

#endif // SMP_H
