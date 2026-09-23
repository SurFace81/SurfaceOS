#ifndef ACPI_H
#define ACPI_H

#include "efi.h"

// ACPI as far as the loader needs it: find the RSDP for the kernel, and find
// the DMAR table to switch the VT-d remapping hardware off before the jump.
//
// Why the loader: with VT-d enabled some firmware leaves DMA translation on
// after ExitBootServices. The kernel knows nothing about the IOMMU, so every
// xHCI DMA gets blocked (Enable Slot times out, no USB device enumerates).
// The loader still runs on the firmware's identity map, so the remapping
// registers are reachable at their physical addresses.

#pragma pack(push, 1)
typedef struct {
    char    Signature[8];       // "RSD PTR "
    UINT8   Checksum;           // first 20 bytes
    char    OemId[6];
    UINT8   Revision;           // 0: ACPI 1.0 (RSDT only), 2+: XSDT
    UINT32  RsdtAddress;
    UINT32  Length;             // revision 2+
    UINT64  XsdtAddress;
    UINT8   ExtendedChecksum;   // whole structure
    UINT8   Reserved[3];
} ACPI_RSDP;

typedef struct {
    char    Signature[4];
    UINT32  Length;
    UINT8   Revision;
    UINT8   Checksum;
    char    OemId[6];
    char    OemTableId[8];
    UINT32  OemRevision;
    UINT32  CreatorId;
    UINT32  CreatorRevision;
} ACPI_SDT_HEADER;
#pragma pack(pop)

// DMAR (Intel VT-d spec, chapter 8). Remapping structures start after the
// 36-byte header, Host Address Width, Flags and 10 reserved bytes.
#define DMAR_STRUCTS_OFFSET     48
#define DMAR_TYPE_DRHD          0
#define DRHD_REG_BASE_OFFSET    8

// Remapping unit registers (VT-d spec, chapter 11)
#define VTD_REG_CAP             0x08
#define VTD_REG_GCMD            0x18
#define VTD_REG_GSTS            0x1C
#define VTD_REG_PMEN            0x64

#define VTD_GSTS_TES            (1U << 31)  // translation enabled
#define VTD_GSTS_IRES           (1U << 25)  // interrupt remapping enabled
// GSTS bits that are persistent state and must be carried over when GCMD is
// written; the rest are one-shot commands (spec 11.4.4).
#define VTD_GSTS_PERSISTENT     0x96FFFFFFU

#define VTD_CAP_PLMR            (1ULL << 5) // protected low-memory region
#define VTD_CAP_PHMR            (1ULL << 6) // protected high-memory region
#define VTD_PMEN_EPM            (1U << 31)  // enable protected memory
#define VTD_PMEN_PRS            (1U << 0)   // protected region status

// Results for SFOS_BOOT_HEADER.DmarFlags
#define DMAR_FLAG_PRESENT       (1U << 0)   // a DMAR table exists
#define DMAR_FLAG_WAS_ENABLED   (1U << 1)   // some unit had TE, IRE or EPM on
#define DMAR_FLAG_TIMEOUT       (1U << 2)   // some unit did not acknowledge

// Polls run after ExitBootServices, so there is no Stall(): bound them by
// iterations. An uncached MMIO read takes well over 100 ns, so this is
// comfortably more than the few microseconds the hardware needs.
#define VTD_POLL_ITERATIONS     10000000

struct EFI_GUID EFI_ACPI_20_TABLE_GUID = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
struct EFI_GUID EFI_ACPI_10_TABLE_GUID = {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

static int acpi_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static int acpi_checksum_ok(const void *p, UINT32 len)
{
    UINT8 sum = 0;
    for (UINT32 i = 0; i < len; i++)
        sum += ((const UINT8 *)p)[i];
    return sum == 0;
}

static int acpi_signature_is(const char *sig, const char *want, int len)
{
    for (int i = 0; i < len; i++)
        if (sig[i] != want[i])
            return 0;
    return 1;
}

// RSDP from the EFI configuration table, ACPI 2.0+ preferred. 0 if none.
static ACPI_RSDP *acpi_find_rsdp(EFI_SYSTEM_TABLE *SystemTable)
{
    ACPI_RSDP *v1 = NULL;

    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &SystemTable->ConfigurationTable[i];
        ACPI_RSDP *rsdp = (ACPI_RSDP *)t->VendorTable;

        if (!rsdp || !acpi_signature_is(rsdp->Signature, "RSD PTR ", 8))
            continue;
        if (!acpi_checksum_ok(rsdp, 20))
            continue;

        if (acpi_guid_equal(&t->VendorGuid, &EFI_ACPI_20_TABLE_GUID) &&
            rsdp->Revision >= 2 && acpi_checksum_ok(rsdp, rsdp->Length))
            return rsdp;
        if (acpi_guid_equal(&t->VendorGuid, &EFI_ACPI_10_TABLE_GUID))
            v1 = rsdp;
    }
    return v1;
}

// First table with signature `sig` listed in the XSDT (or the RSDT on ACPI
// 1.0), with a valid checksum. NULL if there is none.
static ACPI_SDT_HEADER *acpi_find_table(ACPI_RSDP *rsdp, const char *sig)
{
    if (!rsdp)
        return NULL;

    int xsdt = rsdp->Revision >= 2 && rsdp->XsdtAddress;
    ACPI_SDT_HEADER *root = xsdt ? (ACPI_SDT_HEADER *)rsdp->XsdtAddress
                                 : (ACPI_SDT_HEADER *)(UINT64)rsdp->RsdtAddress;
    if (!root || root->Length < sizeof(ACPI_SDT_HEADER) ||
        !acpi_checksum_ok(root, root->Length))
        return NULL;

    UINT32 entry_size = xsdt ? 8 : 4;
    UINT32 count = (root->Length - sizeof(ACPI_SDT_HEADER)) / entry_size;
    UINT8 *entries = (UINT8 *)root + sizeof(ACPI_SDT_HEADER);

    for (UINT32 i = 0; i < count; i++) {
        UINT64 addr = xsdt ? *(UINT64 *)(entries + i * 8)
                           : *(UINT32 *)(entries + i * 4);
        ACPI_SDT_HEADER *t = (ACPI_SDT_HEADER *)addr;
        if (!t || !acpi_signature_is(t->Signature, sig, 4))
            continue;
        if (t->Length >= sizeof(ACPI_SDT_HEADER) && acpi_checksum_ok(t, t->Length))
            return t;
    }
    return NULL;
}

static inline UINT32 vtd_read32(UINT64 base, UINT32 reg)
{
    return *(volatile UINT32 *)(base + reg);
}

static inline UINT64 vtd_read64(UINT64 base, UINT32 reg)
{
    return *(volatile UINT64 *)(base + reg);
}

static inline void vtd_write32(UINT64 base, UINT32 reg, UINT32 val)
{
    *(volatile UINT32 *)(base + reg) = val;
}

// Wait until (reg & mask) == 0. 0 on timeout.
static int vtd_wait_clear(UINT64 base, UINT32 reg, UINT32 mask)
{
    for (UINT32 i = 0; i < VTD_POLL_ITERATIONS; i++) {
        if (!(vtd_read32(base, reg) & mask))
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

// Clear one GSTS-backed enable through GCMD (read GSTS, keep only the
// persistent bits, drop the one we want off) and wait for the ack.
static int vtd_gcmd_clear(UINT64 base, UINT32 status_bit)
{
    UINT32 sts = vtd_read32(base, VTD_REG_GSTS);
    if (!(sts & status_bit))
        return 1;
    vtd_write32(base, VTD_REG_GCMD, (sts & VTD_GSTS_PERSISTENT) & ~status_bit);
    return vtd_wait_clear(base, VTD_REG_GSTS, status_bit);
}

// Turn every DMA remapping unit listed in the DMAR table into pass-through:
// interrupt remapping off, DMA translation off, protected memory regions
// off. The kernel programs none of it, and with any of these left on the
// firmware's settings keep deciding which DMA and MSIs get through.
//
// Must run after ExitBootServices: firmware DMA protection drivers may still
// touch the units until then. Returns DMAR_FLAG_* and counts in *units /
// *disabled (units that had something on and now have everything off).
static UINT32 acpi_disable_dmar(ACPI_RSDP *rsdp, UINT32 *units, UINT32 *disabled)
{
    *units = 0;
    *disabled = 0;

    ACPI_SDT_HEADER *dmar = acpi_find_table(rsdp, "DMAR");
    if (!dmar)
        return 0;

    UINT32 flags = DMAR_FLAG_PRESENT;
    UINT8 *p   = (UINT8 *)dmar + DMAR_STRUCTS_OFFSET;
    UINT8 *end = (UINT8 *)dmar + dmar->Length;

    while (p + 4 <= end) {
        UINT16 type = *(UINT16 *)p;
        UINT16 len  = *(UINT16 *)(p + 2);
        if (len < 4 || p + len > end)
            break;

        if (type == DMAR_TYPE_DRHD && len >= DRHD_REG_BASE_OFFSET + 8) {
            UINT64 base = *(UINT64 *)(p + DRHD_REG_BASE_OFFSET);
            (*units)++;

            UINT32 sts  = vtd_read32(base, VTD_REG_GSTS);
            UINT64 cap  = vtd_read64(base, VTD_REG_CAP);
            int has_pmr = (cap & (VTD_CAP_PLMR | VTD_CAP_PHMR)) != 0;
            UINT32 pmen = has_pmr ? vtd_read32(base, VTD_REG_PMEN) : 0;

            if ((sts & (VTD_GSTS_TES | VTD_GSTS_IRES)) || (pmen & VTD_PMEN_EPM)) {
                flags |= DMAR_FLAG_WAS_ENABLED;

                int ok = vtd_gcmd_clear(base, VTD_GSTS_IRES);
                ok &= vtd_gcmd_clear(base, VTD_GSTS_TES);
                if (pmen & VTD_PMEN_EPM) {
                    vtd_write32(base, VTD_REG_PMEN, pmen & ~VTD_PMEN_EPM);
                    ok &= vtd_wait_clear(base, VTD_REG_PMEN, VTD_PMEN_PRS);
                }

                if (ok)
                    (*disabled)++;
                else
                    flags |= DMAR_FLAG_TIMEOUT;
            }
        }
        p += len;
    }
    return flags;
}

#endif
