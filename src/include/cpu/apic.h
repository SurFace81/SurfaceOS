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

    // This CPU's APIC id.
    uint32_t id();
}

// The local APIC's spurious-interrupt vector (interrupts.asm): no EOI.
#define APIC_SPURIOUS_VECTOR 0xFF
extern "C" void apic_spurious();

#endif // APIC_H
