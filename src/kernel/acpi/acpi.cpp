// src/kernel/acpi/acpi.cpp
#include "../../include/acpi/acpi.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/ports.h"
#include "../../include/drivers/uart.h"
#include "../../include/mm/memory.h"

namespace
{
    struct __attribute__((packed)) rsdp_t
    {
        char     signature[8];      // "RSD PTR "
        uint8_t  checksum;
        char     oem_id[6];
        uint8_t  revision;
        uint32_t rsdt_address;
        uint32_t length;            // revision 2+
        uint64_t xsdt_address;
        uint8_t  extended_checksum;
        uint8_t  reserved[3];
    };

    struct __attribute__((packed)) sdt_header
    {
        char     signature[4];
        uint32_t length;
        uint8_t  revision;
        uint8_t  checksum;
        char     oem_id[6];
        char     oem_table_id[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint32_t creator_revision;
    };

    // Generic Address Structure (ACPI 6.5, 5.2.3.2)
    struct __attribute__((packed)) gas_t
    {
        uint8_t  space;             // 0 memory, 1 I/O, 2 PCI config
        uint8_t  bit_width;
        uint8_t  bit_offset;
        uint8_t  access_size;
        uint64_t address;
    };

    const uint8_t GAS_MEMORY = 0;
    const uint8_t GAS_IO     = 1;
    const uint8_t GAS_PCI    = 2;

    // FADT field offsets (ACPI 6.5, table 5.9). Fields past the table's
    // length do not exist: older revisions stop early.
    const uint32_t FADT_DSDT            = 40;
    const uint32_t FADT_SMI_CMD         = 48;
    const uint32_t FADT_ACPI_ENABLE     = 52;
    const uint32_t FADT_PM1A_EVT_BLK    = 56;
    const uint32_t FADT_PM1B_EVT_BLK    = 60;
    const uint32_t FADT_PM1A_CNT_BLK    = 64;
    const uint32_t FADT_PM1B_CNT_BLK    = 68;
    const uint32_t FADT_PM1_EVT_LEN     = 88;
    const uint32_t FADT_FLAGS           = 112;
    const uint32_t FADT_RESET_REG       = 116;
    const uint32_t FADT_RESET_VALUE     = 128;
    const uint32_t FADT_X_DSDT          = 140;
    const uint32_t FADT_X_PM1A_EVT_BLK  = 148;
    const uint32_t FADT_X_PM1B_EVT_BLK  = 160;
    const uint32_t FADT_X_PM1A_CNT_BLK  = 172;
    const uint32_t FADT_X_PM1B_CNT_BLK  = 184;
    const uint32_t FADT_SLEEP_CTL_REG   = 244;

    const uint32_t FADT_FLAG_RESET_REG_SUP = 1U << 10;
    const uint32_t FADT_FLAG_HW_REDUCED    = 1U << 20;

    // PM1 registers (ACPI 6.5, 4.8.3)
    const uint16_t PM1_STS_WAK       = 1U << 15;
    const uint16_t PM1_CNT_SCI_EN    = 1U << 0;
    const uint16_t PM1_CNT_SLP_TYP   = 7U << 10;
    const uint16_t PM1_CNT_SLP_EN    = 1U << 13;
    const uint8_t  SLEEP_CTL_SLP_EN  = 1U << 5;

    // AML opcodes needed to read the \_S5 package
    const uint8_t AML_ZERO    = 0x00;
    const uint8_t AML_ONE     = 0x01;
    const uint8_t AML_NAME    = 0x08;
    const uint8_t AML_BYTE    = 0x0A;
    const uint8_t AML_WORD    = 0x0B;
    const uint8_t AML_DWORD   = 0x0C;
    const uint8_t AML_PACKAGE = 0x12;
    const uint8_t AML_ONES    = 0xFF;

    const uint32_t MAX_TABLES = 64;

    struct table_entry
    {
        char     sig[4];
        uint64_t phys;
        uint32_t length;
        bool     checksum_ok;
    };

    // A register the kernel pokes, reduced to what the accessors need.
    struct reg_t
    {
        uint8_t  space;
        uint8_t  bytes;             // access width: 1, 2 or 4
        uint64_t address;           // 0: register absent
        uint64_t virt;              // memory space: mapped once, see map_reg()
    };

    bool        present = false;
    table_entry tables[MAX_TABLES];
    uint32_t    num_tables = 0;

    uint32_t    fadt_flags = 0;
    uint32_t    smi_cmd = 0;
    uint8_t     acpi_enable_value = 0;
    reg_t       pm1a_sts, pm1b_sts, pm1a_cnt, pm1b_cnt, sleep_ctl, reset_reg;
    uint8_t     reset_value = 0;

    bool        s5_found = false;
    uint8_t     s5_a = 0, s5_b = 0;

    uint32_t    dmar_units = 0, dmar_disabled = 0, dmar_flags = 0;

    // --- physical memory access ---------------------------------------------

    // Tables normally sit in ACPI reclaim/NVS or reserved RAM, which the
    // direct map covers. Anything past it goes through the device window
    // (uncached, but it is read once).
    const uint8_t* map_phys(uint64_t phys, uint64_t len)
    {
        if (!phys)
            return nullptr;
        if (phys + len <= paging::direct_map_limit())
            return (const uint8_t*)phys_to_virt(phys);
        return (const uint8_t*)paging::map_mmio_region(phys, len);
    }

    bool checksum(const uint8_t* p, uint32_t len)
    {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < len; i++)
            sum += p[i];
        return sum == 0;
    }

    // A whole table, mapped by its own length. nullptr if it does not look
    // like one.
    const sdt_header* map_table(uint64_t phys)
    {
        const sdt_header* h = (const sdt_header*)map_phys(phys, sizeof(sdt_header));
        if (!h || h->length < sizeof(sdt_header))
            return nullptr;
        return (const sdt_header*)map_phys(phys, h->length);
    }

    const sdt_header* find_table(const char* sig)
    {
        for (uint32_t i = 0; i < num_tables; i++)
            if (memory::memcmp((const uint8_t*)tables[i].sig, (const uint8_t*)sig, 4) == 0 &&
                tables[i].checksum_ok)
                return map_table(tables[i].phys);
        return nullptr;
    }

    // --- register access ----------------------------------------------------

    // PCI config space GAS: bus 0, device in bits 47:32, function in 31:16,
    // offset in 15:0 (ACPI 6.5, table 5.1).
    uint32_t pci_cf8(uint64_t address)
    {
        uint32_t dev = (uint32_t)(address >> 32) & 0x1F;
        uint32_t fn  = (uint32_t)(address >> 16) & 0x07;
        uint32_t off = (uint32_t)address & 0xFC;
        return 0x80000000U | (dev << 11) | (fn << 8) | off;
    }

    uint32_t reg_read(const reg_t& r)
    {
        if (!r.address)
            return 0;
        switch (r.space)
        {
            case GAS_IO:
            {
                uint16_t port = (uint16_t)r.address;
                if (r.bytes == 1) return port::byte_in(port);
                if (r.bytes == 2) return port::word_in(port);
                return port::dword_in(port);
            }
            case GAS_MEMORY:
            {
                volatile uint8_t* p = (volatile uint8_t*)r.virt;
                if (!p) return 0;
                if (r.bytes == 1) return *p;
                if (r.bytes == 2) return *(volatile uint16_t*)p;
                return *(volatile uint32_t*)p;
            }
        }
        return 0;
    }

    void reg_write(const reg_t& r, uint32_t value)
    {
        if (!r.address)
            return;
        switch (r.space)
        {
            case GAS_IO:
            {
                uint16_t port = (uint16_t)r.address;
                if (r.bytes == 1)      port::byte_out(port, (uint8_t)value);
                else if (r.bytes == 2) port::word_out(port, (uint16_t)value);
                else                   port::dword_out(port, value);
                break;
            }
            case GAS_MEMORY:
            {
                volatile uint8_t* p = (volatile uint8_t*)r.virt;
                if (!p) break;
                if (r.bytes == 1)      *p = (uint8_t)value;
                else if (r.bytes == 2) *(volatile uint16_t*)p = (uint16_t)value;
                else                   *(volatile uint32_t*)p = value;
                break;
            }
            case GAS_PCI:
            {
                // Only the reset register lives here, and it is a byte.
                port::dword_out(0xCF8, pci_cf8(r.address));
                port::byte_out((uint16_t)(0xCFC + (r.address & 3)), (uint8_t)value);
                break;
            }
        }
    }

    // Memory-space registers get their device-window mapping up front: every
    // map_mmio_region() call takes a fresh 2 MiB slot, and the polls in
    // enable_acpi_mode() read the same register thousands of times.
    void map_reg(reg_t& r)
    {
        if (r.address && r.space == GAS_MEMORY)
            r.virt = paging::map_mmio_region(r.address, r.bytes);
    }

    uint8_t gas_bytes(const gas_t* g, uint8_t fallback)
    {
        if (g->access_size >= 1 && g->access_size <= 3)
            return (uint8_t)(1U << (g->access_size - 1));
        if (g->bit_width == 8)  return 1;
        if (g->bit_width == 16) return 2;
        if (g->bit_width == 32) return 4;
        return fallback;
    }

    // --- FADT ---------------------------------------------------------------

    template <typename T>
    bool fadt_field(const sdt_header* fadt, uint32_t off, T* out)
    {
        if (off + sizeof(T) > fadt->length)
            return false;
        memory::memcpy((uint8_t*)out, (const uint8_t*)fadt + off, sizeof(T));
        return true;
    }

    // A PM1 block: the 64-bit GAS form when the FADT has a usable one, else
    // the legacy 32-bit I/O port. `offset` selects a register inside the
    // block (PM1 event blocks hold STS, then EN).
    reg_t pm1_reg(const sdt_header* fadt, uint32_t x_off, uint32_t legacy_off,
                  uint32_t offset)
    {
        reg_t r = {GAS_IO, 2, 0, 0};

        gas_t g;
        if (fadt_field(fadt, x_off, &g) && g.address)
        {
            r.space = g.space;
            r.address = g.address + offset;
            return r;
        }

        uint32_t port = 0;
        if (fadt_field(fadt, legacy_off, &port) && port)
            r.address = port + offset;
        return r;
    }

    void parse_fadt(const sdt_header* fadt)
    {
        fadt_field(fadt, FADT_FLAGS, &fadt_flags);
        fadt_field(fadt, FADT_SMI_CMD, &smi_cmd);
        fadt_field(fadt, FADT_ACPI_ENABLE, &acpi_enable_value);

        // PM1 status is the first half of the event block; the control block
        // holds only the control register.
        pm1a_sts = pm1_reg(fadt, FADT_X_PM1A_EVT_BLK, FADT_PM1A_EVT_BLK, 0);
        pm1b_sts = pm1_reg(fadt, FADT_X_PM1B_EVT_BLK, FADT_PM1B_EVT_BLK, 0);
        pm1a_cnt = pm1_reg(fadt, FADT_X_PM1A_CNT_BLK, FADT_PM1A_CNT_BLK, 0);
        pm1b_cnt = pm1_reg(fadt, FADT_X_PM1B_CNT_BLK, FADT_PM1B_CNT_BLK, 0);

        uint8_t evt_len = 0;
        fadt_field(fadt, FADT_PM1_EVT_LEN, &evt_len);
        if (evt_len < 4)
        {
            // No event block length means no usable status register.
            pm1a_sts.address = 0;
            pm1b_sts.address = 0;
        }

        gas_t g;
        if (fadt_field(fadt, FADT_SLEEP_CTL_REG, &g) && g.address)
            sleep_ctl = {g.space, 1, g.address, 0};

        if ((fadt_flags & FADT_FLAG_RESET_REG_SUP) &&
            fadt_field(fadt, FADT_RESET_REG, &g) && g.address &&
            fadt_field(fadt, FADT_RESET_VALUE, &reset_value))
            reset_reg = {g.space, gas_bytes(&g, 1), g.address, 0};

        map_reg(pm1a_sts);
        map_reg(pm1b_sts);
        map_reg(pm1a_cnt);
        map_reg(pm1b_cnt);
        map_reg(sleep_ctl);
        map_reg(reset_reg);
    }

    // --- \_S5 ---------------------------------------------------------------

    // One AML integer: ZeroOp, OneOp, OnesOp or a Byte/Word/DWord constant.
    bool aml_integer(const uint8_t*& p, const uint8_t* end, uint32_t* out)
    {
        if (p >= end)
            return false;
        switch (*p++)
        {
            case AML_ZERO: *out = 0; return true;
            case AML_ONE:  *out = 1; return true;
            case AML_ONES: *out = 0xFFFFFFFF; return true;
            case AML_BYTE:
                if (p + 1 > end) return false;
                *out = p[0]; p += 1; return true;
            case AML_WORD:
                if (p + 2 > end) return false;
                *out = p[0] | (p[1] << 8); p += 2; return true;
            case AML_DWORD:
                if (p + 4 > end) return false;
                *out = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4; return true;
        }
        return false;
    }

    // Find `Name (_S5, Package () { SLP_TYPa, SLP_TYPb, ... })` by pattern:
    // NameOp, optional root prefix, "_S5_", PackageOp, PkgLength,
    // NumElements, then the integers. Firmware that computes _S5 in a method
    // is out of reach without an interpreter.
    bool find_s5(const sdt_header* table)
    {
        const uint8_t* body = (const uint8_t*)table + sizeof(sdt_header);
        const uint8_t* end  = (const uint8_t*)table + table->length;

        for (const uint8_t* p = body + 1; p + 5 < end; p++)
        {
            if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_')
                continue;

            bool named = p[-1] == AML_NAME ||
                         (p[-1] == '\\' && p - 2 >= body && p[-2] == AML_NAME);
            if (!named || p[4] != AML_PACKAGE)
                continue;

            const uint8_t* q = p + 5;
            uint8_t pkg_len_bytes = (uint8_t)((*q >> 6) + 1);
            q += pkg_len_bytes;
            if (q + 1 > end)
                return false;
            q++;                                // NumElements

            uint32_t a = 0, b = 0;
            if (!aml_integer(q, end, &a))
                return false;
            if (!aml_integer(q, end, &b))
                b = 0;                          // one-element package: PM1b unused

            s5_a = (uint8_t)(a & 7);
            s5_b = (uint8_t)(b & 7);
            return true;
        }
        return false;
    }

    // --- mode switch and resets ---------------------------------------------

    bool hardware_reduced_flag()
    {
        return (fadt_flags & FADT_FLAG_HW_REDUCED) != 0;
    }

    void io_delay(uint32_t us)
    {
        for (uint32_t i = 0; i < us; i++)
            port::io_wait();
    }

    // Legacy-mode firmware owns the PM registers until SMI_CMD hands them
    // over. UEFI machines are already in ACPI mode, and hardware-reduced
    // ones have no such mode at all.
    void enable_acpi_mode()
    {
        if (hardware_reduced_flag())
            return;
        if (reg_read(pm1a_cnt) & PM1_CNT_SCI_EN)
            return;
        if (!smi_cmd || !acpi_enable_value)
            return;

        port::byte_out((uint16_t)smi_cmd, acpi_enable_value);
        for (uint32_t i = 0; i < 3000; i++)     // ~3 s, spec allows a while
        {
            if (reg_read(pm1a_cnt) & PM1_CNT_SCI_EN)
                return;
            io_delay(1000);
        }
        uart::printf("acpi: firmware did not switch to ACPI mode\n");
    }

    void reset_8042()
    {
        for (uint32_t i = 0; i < 100000; i++)
            if (!(port::byte_in(0x64) & 0x02))  // input buffer empty
                break;
        port::byte_out(0x64, 0xFE);             // pulse the reset line
    }

    [[noreturn]] void triple_fault()
    {
        struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = {0, 0};
        asm volatile("lidt %0; int3" :: "m"(idtr));
        for (;;)
            asm volatile("cli; hlt");
    }
}

namespace acpi
{
    void init(const BOOT_HEADER* boot_header)
    {
        dmar_units    = boot_header->DmarUnits;
        dmar_disabled = boot_header->DmarDisabled;
        dmar_flags    = boot_header->DmarFlags;

        uint64_t rsdp_phys = boot_header->AcpiRsdpAddress;
        const rsdp_t* rsdp = (const rsdp_t*)map_phys(rsdp_phys, sizeof(rsdp_t));
        if (!rsdp || memory::memcmp((const uint8_t*)rsdp->signature,
                                    (const uint8_t*)"RSD PTR ", 8) != 0 ||
            !checksum((const uint8_t*)rsdp, 20))
        {
            uart::printf("acpi: no valid RSDP\n");
            return;
        }

        bool xsdt = rsdp->revision >= 2 && rsdp->xsdt_address;
        uint64_t root_phys = xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
        const sdt_header* root = map_table(root_phys);
        if (!root || !checksum((const uint8_t*)root, root->length))
        {
            uart::printf("acpi: bad %s at %llx\n", xsdt ? "XSDT" : "RSDT", root_phys);
            return;
        }

        uint32_t entry_size = xsdt ? 8 : 4;
        uint32_t count = (root->length - sizeof(sdt_header)) / entry_size;
        const uint8_t* entries = (const uint8_t*)root + sizeof(sdt_header);

        for (uint32_t i = 0; i < count && num_tables < MAX_TABLES; i++)
        {
            uint64_t phys = 0;
            memory::memcpy((uint8_t*)&phys, entries + i * entry_size, entry_size);
            const sdt_header* t = map_table(phys);
            if (!t)
                continue;

            table_entry& e = tables[num_tables++];
            memory::memcpy((uint8_t*)e.sig, (const uint8_t*)t->signature, 4);
            e.phys = phys;
            e.length = t->length;
            e.checksum_ok = checksum((const uint8_t*)t, t->length);
        }
        present = true;

        const sdt_header* fadt = find_table("FACP");
        if (fadt)
        {
            parse_fadt(fadt);

            // The DSDT is not in the XSDT; only the FADT points at it.
            uint64_t dsdt_phys = 0;
            uint32_t dsdt32 = 0;
            if (!fadt_field(fadt, FADT_X_DSDT, &dsdt_phys) || !dsdt_phys)
            {
                fadt_field(fadt, FADT_DSDT, &dsdt32);
                dsdt_phys = dsdt32;
            }
            const sdt_header* dsdt = map_table(dsdt_phys);
            if (dsdt && checksum((const uint8_t*)dsdt, dsdt->length))
                s5_found = find_s5(dsdt);
        }

        // Some firmware defines \_S5 in an SSDT instead.
        for (uint32_t i = 0; !s5_found && i < num_tables; i++)
            if (memory::memcmp((const uint8_t*)tables[i].sig, (const uint8_t*)"SSDT", 4) == 0 &&
                tables[i].checksum_ok)
                s5_found = find_s5(map_table(tables[i].phys));

        uart::printf("acpi: rev %u, %u tables, FADT %s, _S5 %s (%u/%u), reset reg %s%s\n",
                     (uint32_t)rsdp->revision, num_tables,
                     fadt ? "ok" : "missing",
                     s5_found ? "found" : "missing", (uint32_t)s5_a, (uint32_t)s5_b,
                     reset_reg.address ? "yes" : "no",
                     hardware_reduced_flag() ? ", hardware-reduced" : "");
    }

    bool available()
    {
        return present;
    }

    uint32_t table_count()
    {
        return num_tables;
    }

    bool table_info(uint32_t index, char sig[5], uint64_t* phys,
                    uint32_t* length, bool* checksum_ok)
    {
        if (index >= num_tables)
            return false;
        memory::memcpy((uint8_t*)sig, (const uint8_t*)tables[index].sig, 4);
        sig[4] = '\0';
        *phys = tables[index].phys;
        *length = tables[index].length;
        *checksum_ok = tables[index].checksum_ok;
        return true;
    }

    bool s5_values(uint8_t* slp_typ_a, uint8_t* slp_typ_b)
    {
        *slp_typ_a = s5_a;
        *slp_typ_b = s5_b;
        return s5_found;
    }

    bool hardware_reduced()
    {
        return hardware_reduced_flag();
    }

    bool has_reset_register()
    {
        return reset_reg.address != 0;
    }

    void dmar_status(uint32_t* units, uint32_t* disabled, uint32_t* flags)
    {
        *units = dmar_units;
        *disabled = dmar_disabled;
        *flags = dmar_flags;
    }

    void reboot()
    {
        asm volatile("cli");
        uart::printf("acpi: reboot\n");

        // 1. The FADT reset register: the method the firmware vouches for.
        if (reset_reg.address)
        {
            reg_write(reset_reg, reset_value);
            io_delay(50000);
        }

        // 2. The PCI reset control register (every Intel chipset since the
        //    PIIX): system reset, then full reset.
        port::byte_out(0xCF9, 0x02);
        io_delay(10);
        port::byte_out(0xCF9, 0x06);
        io_delay(50000);

        // 3. The keyboard controller's reset line.
        reset_8042();
        io_delay(50000);

        // 4. No IDT: the next exception triple-faults and the CPU resets.
        triple_fault();
    }

    void shutdown()
    {
        if (!s5_found)
        {
            uart::printf("acpi: shutdown impossible, no \\_S5 package\n");
            return;
        }

        asm volatile("cli");
        uart::printf("acpi: entering S5 (SLP_TYP %u/%u)\n", (uint32_t)s5_a, (uint32_t)s5_b);
        asm volatile("wbinvd" ::: "memory");

        if (hardware_reduced_flag())
        {
            if (!sleep_ctl.address)
                return;
            reg_write(sleep_ctl, (uint8_t)(((s5_a & 7) << 2) | SLEEP_CTL_SLP_EN));
            io_delay(100000);
            return;
        }

        if (!pm1a_cnt.address)
            return;

        enable_acpi_mode();

        // WAK_STS is write-1-to-clear; a stale one can make the platform
        // wake straight back up.
        reg_write(pm1a_sts, PM1_STS_WAK);
        reg_write(pm1b_sts, PM1_STS_WAK);

        // Program SLP_TYP first, then set SLP_EN in a separate write, the
        // way the reference implementation (ACPICA) does it.
        uint16_t a = (uint16_t)(reg_read(pm1a_cnt) & ~(PM1_CNT_SLP_TYP | PM1_CNT_SLP_EN));
        uint16_t b = (uint16_t)(reg_read(pm1b_cnt) & ~(PM1_CNT_SLP_TYP | PM1_CNT_SLP_EN));
        a |= (uint16_t)(s5_a << 10);
        b |= (uint16_t)(s5_b << 10);

        reg_write(pm1a_cnt, a);
        reg_write(pm1b_cnt, b);
        reg_write(pm1a_cnt, a | PM1_CNT_SLP_EN);
        reg_write(pm1b_cnt, b | PM1_CNT_SLP_EN);

        io_delay(100000);
        uart::printf("acpi: still running after S5 request\n");
    }
} // namespace acpi
