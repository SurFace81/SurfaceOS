#ifndef ACPI_H
#define ACPI_H

#include "../cpu/types.h"
#include "../boot/boot.h"

// ACPI without an AML interpreter: the static tables (RSDP -> XSDT/RSDT ->
// FADT, DSDT) and the one AML object that power-off needs, \_S5. Enough for
// reboot and shutdown; anything that has to run AML methods (batteries,
// thermal zones, S3) needs a real interpreter.

namespace acpi
{
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
