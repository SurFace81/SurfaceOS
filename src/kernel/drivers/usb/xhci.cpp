#include "../../../include/drivers/usb/xhci.h"

// Simple busy-wait delay (no HPET/ACPI needed)
static void delay_ms(uint32_t ms) {
    // ~1ms at roughly 1GHz; imprecise but sufficient for xHCI timeouts
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 100000; j++) {
            asm volatile("pause");
        }
    }
}

// Map MMIO BAR (identity mapping)
static uintptr_t xhci_map_mmio(uint64_t bar_addr, uint64_t bar_size) {
    return (uintptr_t)paging::map_mmio_region(bar_addr, bar_size);
}

static uint64_t get_bar_size_64(PCIDevice* dev, int bar_index) {
    uint8_t off_lo = PCI_BAR0 + bar_index * 4;
    uint8_t off_hi = PCI_BAR0 + (bar_index + 1) * 4;

    uint32_t orig_lo = pci::read32(dev, off_lo);
    uint32_t orig_hi = pci::read32(dev, off_hi);

    // Check if 64-bit BAR (type field bits 2:1 == 0x02)
    bool is_64bit = ((orig_lo >> 1) & 0x3) == 0x02;

    // Write all 1s to determine size
    pci::write32(dev, off_lo, 0xFFFFFFFF);
    uint32_t size_lo = pci::read32(dev, off_lo);
    pci::write32(dev, off_lo, orig_lo); // restore

    uint64_t size_mask;
    if (is_64bit) {
        pci::write32(dev, off_hi, 0xFFFFFFFF);
        uint32_t size_hi = pci::read32(dev, off_hi);
        pci::write32(dev, off_hi, orig_hi); // restore

        size_mask = ((uint64_t)size_hi << 32) | (size_lo & 0xFFFFFFF0);
    } else {
        size_mask = (uint64_t)(size_lo & 0xFFFFFFF0);
        // Sign-extend for 32-bit
        size_mask |= 0xFFFFFFFF00000000ULL;
    }

    return (~size_mask) + 1;
}

// State
static volatile xhci_cap_regs* cap_regs = nullptr;
static volatile xhci_op_regs*  op_regs  = nullptr;
static uintptr_t xhc_base = 0;

// Parsed capability fields
static uint8_t  max_device_slots;
static uint8_t  max_interrupters_val;
static uint8_t  max_ports;
static uint8_t  ist;
static uint8_t  erst_max;
static uint8_t  max_scratchpad_buffers;
static bool     ac64;
static bool     csz;
static bool     ppc;
static bool     pind;
static bool     lhrc;
static uint32_t xecp_offset;

static void parse_cap_regs() {
    cap_regs = (volatile xhci_cap_regs*)xhc_base;

    max_device_slots      = XHCI_MAX_DEVICE_SLOTS(cap_regs);
    max_interrupters_val  = XHCI_MAX_INTERRUPTERS(cap_regs);
    max_ports             = XHCI_MAX_PORTS(cap_regs);
    ist                   = XHCI_IST(cap_regs);
    erst_max              = XHCI_ERST_MAX(cap_regs);
    max_scratchpad_buffers = XHCI_MAX_SCRATCHPAD_BUFFERS(cap_regs);
    ac64                  = XHCI_AC64(cap_regs);
    csz                   = XHCI_CSZ(cap_regs);
    ppc                   = XHCI_PPC(cap_regs);
    pind                  = XHCI_PIND(cap_regs);
    lhrc                  = XHCI_LHRC(cap_regs);
    xecp_offset           = XHCI_XECP(cap_regs) * sizeof(uint32_t);

    op_regs = (volatile xhci_op_regs*)(xhc_base + cap_regs->caplength);
}

static void log_cap_regs() {
    uart::printf("xHCI Capability Registers:\n");
    uart::printf("  caplength       : %u\n",  (uint32_t)cap_regs->caplength);
    uart::printf("  hciversion      : %x\n",  (uint32_t)cap_regs->hciversion);
    uart::printf("  max_device_slots: %u\n",  (uint32_t)max_device_slots);
    uart::printf("  max_interrupters: %u\n",  (uint32_t)max_interrupters_val);
    uart::printf("  max_ports       : %u\n",  (uint32_t)max_ports);
    uart::printf("  IST             : %u\n",  (uint32_t)ist);
    uart::printf("  ERST max        : %u\n",  (uint32_t)erst_max);
    uart::printf("  scratchpad bufs : %u\n",  (uint32_t)max_scratchpad_buffers);
    uart::printf("  64-bit addr     : %s\n",  ac64 ? "yes" : "no");
    uart::printf("  64-byte ctx     : %s\n",  csz  ? "yes" : "no");
    uart::printf("  port power ctrl : %s\n",  ppc  ? "yes" : "no");
    uart::printf("  port indicators : %s\n",  pind ? "yes" : "no");
    uart::printf("  light reset     : %s\n",  lhrc ? "yes" : "no");
    uart::printf("  xECP offset     : %x\n",  xecp_offset);
}

static void log_op_regs() {
    uart::printf("xHCI Operational Registers:\n");
    uart::printf("  usbcmd  : %x\n",  op_regs->usbcmd);
    uart::printf("  usbsts  : %x\n",  op_regs->usbsts);
    uart::printf("  pagesize: %x\n",  op_regs->pagesize);
    uart::printf("  dnctrl  : %x\n",  op_regs->dnctrl);
    uart::printf("  crcr    : %llx\n", op_regs->crcr);
    uart::printf("  dcbaap  : %llx\n", op_regs->dcbaap);
    uart::printf("  config  : %x\n",  op_regs->config);
}

static bool take_ownership_from_bios() {
    if (xecp_offset == 0)
        return true;

    volatile uint32_t* ecap = (volatile uint32_t*)(xhc_base + xecp_offset);

    // Walk extended capability list looking for legacy support
    while (true) {
        uint32_t val = *ecap;
        uint8_t cap_id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (cap_id == XHCI_LEGACY_SUPPORT_CAP_ID) {
            // Set OS owned semaphore
            *ecap = val | XHCI_LEGACY_OS_OWNED;

            // Wait for BIOS to release ownership
            uint32_t timeout = 500;
            while ((*ecap & XHCI_LEGACY_BIOS_OWNED) && timeout > 0) {
                delay_ms(1);
                timeout--;
            }

            if (*ecap & XHCI_LEGACY_BIOS_OWNED) {
                uart::printf("xhci: BIOS did not release ownership\n");
                return false;
            }

            // Disable legacy SMI interrupts
            volatile uint32_t* leg_ctrl = ecap + 1;
            *leg_ctrl &= ~(uint32_t)0x0000E01F; // clear SMI enable bits

            uart::printf("xhci: took ownership from BIOS\n");
            return true;
        }

        if (next == 0)
            break;
        ecap = XHCI_NEXT_EXT_CAP_PTR(ecap, next);
    }

    // No legacy support cap found — no BIOS handoff needed
    return true;
}

static bool reset_controller() {
    // Stop the controller
    uint32_t cmd = op_regs->usbcmd;
    cmd &= ~XHCI_USBCMD_RUN_STOP;
    op_regs->usbcmd = cmd;

    // Wait for HCHalted
    uint32_t timeout = 200;
    while (!(op_regs->usbsts & XHCI_USBSTS_HCH)) {
        if (--timeout == 0) {
            uart::printf("xhci: controller did not halt\n");
            return false;
        }
        delay_ms(1);
    }

    // Issue reset
    cmd = op_regs->usbcmd;
    cmd |= XHCI_USBCMD_HCRESET;
    op_regs->usbcmd = cmd;

    // Wait for HCRESET to clear and CNR to clear
    timeout = 1000;
    while ((op_regs->usbcmd & XHCI_USBCMD_HCRESET) ||
           (op_regs->usbsts & XHCI_USBSTS_CNR)) {
        if (--timeout == 0) {
            uart::printf("xhci: controller did not reset\n");
            return false;
        }
        delay_ms(1);
    }

    delay_ms(50);

    // Verify reset defaults
    if (op_regs->usbcmd != 0 || op_regs->dnctrl != 0 ||
        op_regs->crcr != 0 || op_regs->dcbaap != 0 || op_regs->config != 0) {
        uart::printf("xhci: unexpected register values after reset\n");
        return false;
    }

    uart::printf("xhci: controller reset successful\n");
    return true;
}

namespace xhci {
    bool init() {
        PCIDevice* dev = pci::find(PCI_CLASS_SERIAL, 0x03, 0x30);
        if (!dev) {
            uart::printf("xhci: no xHCI controller found\n");
            return false;
        }

        uart::printf("xhci: found controller %x:%x at %u:%u.%u\n",
            (uint32_t)dev->vendor_id, (uint32_t)dev->device_id,
            (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);

        pci::enable_device(dev);

        PCIBar bar = pci::get_bar(dev, 0);
        if (!bar.valid || bar.is_io) {
            uart::printf("xhci: invalid BAR0\n");
            return false;
        }

        uint64_t bar_size = get_bar_size_64(dev, 0);

        uart::printf("xhci: BAR0 phys=%llx size=%llx\n", bar.base, bar_size);

        xhc_base = xhci_map_mmio(bar.base, bar_size);
        uart::printf("xhci: mapped to virt=%llx\n", (uint64_t)xhc_base);

        parse_cap_regs();
        log_cap_regs();

        if (!take_ownership_from_bios())
            return false;

        if (!reset_controller())
            return false;

        log_op_regs();

        return true;
    }
}