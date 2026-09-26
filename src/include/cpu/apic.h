#ifndef APIC_H
#define APIC_H

#include "types.h"

// The local APIC of this CPU and the IOAPICs (from the MADT) in place of
// the 8259 PICs. ISA IRQ n keeps vector IRQ_BASE + n, so the IRQ handlers
// do not change: the IOAPIC routes it - to the GSI the MADT's overrides
// name, with their polarity and trigger mode - to this CPU.
//
// The local APIC is used in whichever mode the firmware left it: xAPIC
// (registers in memory) or x2APIC (registers in MSRs).

namespace apic
{
    // Take interrupt delivery over from the 8259s (which stay masked).
    // False - and nothing changed - without a MADT, a local APIC or an
    // IOAPIC: the 8259s go on as before.
    bool init();
    bool active();

    // End of interrupt, to the local APIC.
    void eoi();

    // Let ISA IRQ `irq` through, or stop it.
    void unmask_irq(uint8_t irq);
    void mask_irq(uint8_t irq);

    // The boot CPU's APIC id.
    uint32_t id();

    // Every other CPU as it starts: turn its own local APIC on, in the
    // boot CPU's mode, and return its APIC id.
    uint32_t init_cpu();

    // Start CPU `apic_id`: an INIT, then a startup IPI that makes it run
    // real-mode code at physical page `page` (below 1 MiB).
    void send_init(uint32_t apic_id);
    void send_startup(uint32_t apic_id, uint64_t page);

    // Let the local APIC's timer drive the tick instead of the PIT. It is
    // calibrated against the running PIT to the same rate, so pit::ticks()
    // and everything measured in them stay as they are; then the PIT's IRQ
    // is masked. False (the PIT goes on) when the APIC is not in use.
    bool start_timer();

    // The same periodic timer on the CPU this runs on (every other CPU as
    // it starts), at the rate start_timer found. Only the boot CPU's tick
    // is the system's clock; the others preempt and wake their own CPU.
    void start_timer_cpu();
}

// The local APIC's spurious-interrupt vector (interrupts.asm): no EOI.
#define APIC_SPURIOUS_VECTOR 0xFF
extern "C" void apic_spurious();

#endif // APIC_H
