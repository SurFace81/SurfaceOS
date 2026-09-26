#ifndef SMP_H
#define SMP_H

#include "types.h"

// The other CPUs. The boot CPU starts each one the MADT lists (INIT, then
// startup IPIs into ap_trampoline.asm at the page the loader claimed); a
// started CPU sets up its own GDT, TSS, GS base, IDT, features, syscall
// MSRs and local APIC, says it is online and halts. It runs no kernel code
// after that yet.

namespace smp
{
    // Start the other CPUs; needs the APIC and a ticking timer.
    // `trampoline` is BOOT_HEADER.ApTrampolineAddress (0: stay on one CPU).
    void start(uint64_t trampoline);

    // CPUs running, the boot CPU included.
    uint32_t running();
}

#endif // SMP_H
