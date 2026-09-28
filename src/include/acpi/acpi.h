#ifndef ACPI_H
#define ACPI_H

#include "../cpu/types.h"
#include "../boot/boot.h"

// ACPI without an AML interpreter: the static tables (RSDP -> XSDT/RSDT ->
// FADT, DSDT, MADT) and the one AML object that power-off needs, \_S5.
// Enough for reboot, shutdown and finding the CPUs and interrupt
// controllers; anything that has to run AML methods (batteries, thermal
// zones, S3) needs a real interpreter.

namespace acpi
{
    const uint32_t MAX_CPUS      = 64;
    const uint32_t MAX_IOAPICS   = 8;
    const uint32_t MAX_OVERRIDES = 16;     // one per ISA IRQ at most

    // What the MADT ("APIC") says about the CPUs and interrupt controllers.
    struct cpu_info
    {
        uint32_t apic_id;           // its local APIC's id (x2APIC: 32 bits)
        uint32_t acpi_uid;
        bool     enabled;           // running, or ready to be started
        bool     online_capable;    // disabled now, but may be started
    };

    struct ioapic_info
    {
        uint32_t id;
        uint64_t address;           // physical, 4 KiB of registers
        uint32_t gsi_base;          // first global system interrupt it serves
    };

    // An ISA IRQ that does not arrive on the GSI of the same number, or not
    // edge-triggered active-high as ISA IRQs are by default.
    struct irq_override
    {
        uint8_t  irq;
        uint32_t gsi;
        bool     active_low;
        bool     level;
    };

    struct madt_info
    {
        bool         present;
        uint64_t     lapic_address; // physical
        bool         has_8259;      // PCAT_COMPAT: the legacy PICs are there too
        uint32_t     cpu_count;
        cpu_info     cpus[MAX_CPUS];
        uint32_t     ioapic_count;
        ioapic_info  ioapics[MAX_IOAPICS];
        uint32_t     override_count;
        irq_override overrides[MAX_OVERRIDES];
    };

    // Parsed by init(); present is false without a MADT.
    const madt_info* madt();

    // Walk the tables from the RSDP the loader found. Harmless when there is
    // none: available() stays false and reboot() uses the legacy fallbacks.
    // Needs paging and uart; tables above the direct map go through the
    // device window.
    void        init(const BOOT_HEADER* boot_header);
    bool        available();

    // Tables listed in the XSDT/RSDT, in order. False past the end.
    // `sig` gets the 4-character signature plus a terminator.
    uint32_t    table_count();
    bool        table_info(uint32_t index, char sig[5], uint64_t* phys,
                           uint32_t* length, bool* checksum_ok);

    // SLP_TYPa/SLP_TYPb from \_S5. False if the package was not found, in
    // which case shutdown() cannot work.
    bool        s5_values(uint8_t* slp_typ_a, uint8_t* slp_typ_b);
    bool        hardware_reduced();
    bool        has_reset_register();

    // What the loader did to VT-d (BOOT_DMAR_* flags).
    void        dmar_status(uint32_t* units, uint32_t* disabled, uint32_t* flags);

    // Reset the machine: FADT RESET_REG, then port 0xCF9, then the 8042,
    // then a triple fault. Does not return.
    [[noreturn]] void reboot();

    // Enter S5 (soft off). Interrupts are disabled on the way in. Returns
    // only if the machine is still running afterwards: no \_S5, no PM1
    // control block, or the platform ignored the write. Callers flush their
    // file systems first.
    void        shutdown();
} // namespace acpi

#endif // ACPI_H
