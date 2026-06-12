#include "../../../include/drivers/usb/xhci.h"

// Busy-wait delay
static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++)
        for (volatile uint32_t j = 0; j < 100000; j++)
            asm volatile("pause");
}

// Aligned memory allocator for xHCI structures.
// Allocates extra space to guarantee alignment and boundary constraints.
// Stores the original kmalloc pointer right before the aligned block
// so kfree can recover it.
static void* alloc_xhci_memory(size_t size, size_t alignment, size_t boundary) {
    if (size == 0 || alignment == 0) {
        uart::printf("xhci: bad alloc params size=%u align=%u\n",
                     (uint32_t)size, (uint32_t)alignment);
        while (1) asm volatile("hlt");
    }

    // Worst case: need alignment + boundary padding + space to stash original ptr
    size_t total = size + alignment + boundary + sizeof(void*);
    void* raw = kmalloc(total);
    if (!raw) {
        uart::printf("xhci: alloc failed size=%u\n", (uint32_t)size);
        while (1) asm volatile("hlt");
    }

    // Leave room for storing original pointer
    uintptr_t base = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (base + alignment - 1) & ~(alignment - 1);

    // Check boundary crossing
    if (boundary > 0) {
        uintptr_t start_region = aligned / boundary;
        uintptr_t end_region = (aligned + size - 1) / boundary;
        if (start_region != end_region) {
            // Jump to next boundary
            aligned = (end_region * boundary + alignment - 1) & ~(alignment - 1);
        }
    }

    // Store original pointer for later freeing
    ((void**)aligned)[-1] = raw;

    memory::memset((uint8_t*)aligned, 0, size);
    return (void*)aligned;
}

static void free_xhci_memory(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    kfree(raw);
}

// Physical address (identity mapping through paging)
static uintptr_t xhci_virt_to_phys(void* vaddr) {
    return paging::get_phys_addr((uint64_t)vaddr);
}

// Map MMIO region
static uintptr_t xhci_map_mmio(uint64_t bar_addr, uint64_t bar_size) {
    return (uintptr_t)paging::map_mmio_region(bar_addr, bar_size);
}

// Read BAR size properly for 32-bit and 64-bit BARs
static uint64_t get_bar_size_64(PCIDevice* dev, int bar_index) {
    uint8_t off_lo = PCI_BAR0 + bar_index * 4;
    uint8_t off_hi = PCI_BAR0 + (bar_index + 1) * 4;

    uint32_t orig_lo = pci::read32(dev, off_lo);
    uint32_t orig_hi = pci::read32(dev, off_hi);

    bool is_64bit = ((orig_lo >> 1) & 0x3) == 0x02;

    pci::write32(dev, off_lo, 0xFFFFFFFF);
    uint32_t size_lo = pci::read32(dev, off_lo);
    pci::write32(dev, off_lo, orig_lo);

    uint64_t size_mask;
    if (is_64bit) {
        pci::write32(dev, off_hi, 0xFFFFFFFF);
        uint32_t size_hi = pci::read32(dev, off_hi);
        pci::write32(dev, off_hi, orig_hi);
        size_mask = ((uint64_t)size_hi << 32) | (size_lo & 0xFFFFFFF0);
    } else {
        size_mask = (uint64_t)(size_lo & 0xFFFFFFF0);
        size_mask |= 0xFFFFFFFF00000000ULL;
    }

    return (~size_mask) + 1;
}

// Driver state
static volatile xhci_cap_regs* cap_regs = nullptr;
static volatile xhci_op_regs*  op_regs  = nullptr;
static uintptr_t xhc_base = 0;

static uint8_t  max_device_slots;
static uint8_t  max_interrupters_val;
static uint8_t  max_ports;
static uint8_t  ist;
static uint8_t  erst_max;
static uint8_t  max_scratchpad_bufs;
static bool     ac64;
static bool     csz;
static bool     ppc;
static bool     pind;
static bool     lhrc;
static uint32_t xecp_offset;

// DCBAA (Device Context Base Address Array)
static uint64_t* dcbaa = nullptr;
// Mirror array holding virtual addresses of device contexts
static uint64_t* dcbaa_virt = nullptr;

static void parse_cap_regs() {
    cap_regs = (volatile xhci_cap_regs*)xhc_base;

    max_device_slots     = XHCI_MAX_DEVICE_SLOTS(cap_regs);
    max_interrupters_val = XHCI_MAX_INTERRUPTERS(cap_regs);
    max_ports            = XHCI_MAX_PORTS(cap_regs);
    ist                  = XHCI_IST(cap_regs);
    erst_max             = XHCI_ERST_MAX(cap_regs);
    max_scratchpad_bufs  = XHCI_MAX_SCRATCHPAD_BUFFERS(cap_regs);
    ac64                 = XHCI_AC64(cap_regs);
    csz                  = XHCI_CSZ(cap_regs);
    ppc                  = XHCI_PPC(cap_regs);
    pind                 = XHCI_PIND(cap_regs);
    lhrc                 = XHCI_LHRC(cap_regs);
    xecp_offset          = XHCI_XECP(cap_regs) * sizeof(uint32_t);

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
    uart::printf("  scratchpad bufs : %u\n",  (uint32_t)max_scratchpad_bufs);
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

    while (true) {
        uint32_t val = *ecap;
        uint8_t cap_id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (cap_id == XHCI_LEGACY_SUPPORT_CAP_ID) {
            *ecap = val | XHCI_LEGACY_OS_OWNED;

            uint32_t timeout = 500;
            while ((*ecap & XHCI_LEGACY_BIOS_OWNED) && timeout > 0) {
                delay_ms(1);
                timeout--;
            }

            if (*ecap & XHCI_LEGACY_BIOS_OWNED) {
                uart::printf("xhci: BIOS did not release ownership\n");
                return false;
            }

            volatile uint32_t* leg_ctrl = ecap + 1;
            *leg_ctrl &= ~(uint32_t)0x0000E01F;

            uart::printf("xhci: took ownership from BIOS\n");
            return true;
        }

        if (next == 0)
            break;
        ecap = XHCI_NEXT_EXT_CAP_PTR(ecap, next);
    }

    return true;
}

static bool reset_controller() {
    uint32_t cmd = op_regs->usbcmd;
    cmd &= ~XHCI_USBCMD_RUN_STOP;
    op_regs->usbcmd = cmd;

    uint32_t timeout = 200;
    while (!(op_regs->usbsts & XHCI_USBSTS_HCH)) {
        if (--timeout == 0) {
            uart::printf("xhci: controller did not halt\n");
            return false;
        }
        delay_ms(1);
    }

    cmd = op_regs->usbcmd;
    cmd |= XHCI_USBCMD_HCRESET;
    op_regs->usbcmd = cmd;

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

    if (op_regs->usbcmd != 0 || op_regs->dnctrl != 0 ||
        op_regs->crcr != 0 || op_regs->dcbaap != 0 || op_regs->config != 0) {
        uart::printf("xhci: unexpected register values after reset\n");
        return false;
    }

    uart::printf("xhci: controller reset successful\n");
    return true;
}

// NEW: Setup Device Context Base Address Array
static void setup_dcbaa() {
    size_t dcbaa_size = sizeof(uint64_t) * (max_device_slots + 1);

    dcbaa = (uint64_t*)alloc_xhci_memory(dcbaa_size,
                                          XHCI_DCBAA_ALIGNMENT,
                                          XHCI_DCBAA_BOUNDARY);

    // Virtual address mirror (for driver's own bookkeeping)
    dcbaa_virt = (uint64_t*)kmalloc(sizeof(uint64_t) * (max_device_slots + 1));
    memory::memset((uint8_t*)dcbaa_virt, 0, sizeof(uint64_t) * (max_device_slots + 1));

    uart::printf("xhci: DCBAA virt=%llx phys=%llx slots=%u\n",
                 (uint64_t)dcbaa,
                 (uint64_t)xhci_virt_to_phys(dcbaa),
                 (uint32_t)max_device_slots);

    // Allocate scratchpad buffers if the controller requires them
    if (max_scratchpad_bufs > 0) {
        uart::printf("xhci: allocating %u scratchpad buffers\n",
                     (uint32_t)max_scratchpad_bufs);

        // Scratchpad buffer array: array of physical pointers to each scratchpad page
        uint64_t* sp_array = (uint64_t*)alloc_xhci_memory(
            max_scratchpad_bufs * sizeof(uint64_t),
            XHCI_DCBAA_ALIGNMENT,
            XHCI_DCBAA_BOUNDARY
        );

        // Allocate individual scratchpad pages
        for (uint32_t i = 0; i < max_scratchpad_bufs; i++) {
            void* sp_page = alloc_xhci_memory(
                4096,  // xHCI pagesize register says 4KB
                XHCI_SCRATCHPAD_BUF_ALIGNMENT,
                XHCI_SCRATCHPAD_BUF_BOUNDARY
            );

            uint64_t sp_phys = xhci_virt_to_phys(sp_page);
            sp_array[i] = sp_phys;

            uart::printf("xhci: scratchpad[%u] virt=%llx phys=%llx\n",
                         i, (uint64_t)sp_page, sp_phys);
        }

        uint64_t sp_array_phys = xhci_virt_to_phys(sp_array);

        // DCBAA[0] points to scratchpad buffer array (physical address)
        dcbaa[0] = sp_array_phys;
        dcbaa_virt[0] = (uint64_t)sp_array;

        uart::printf("xhci: scratchpad array virt=%llx phys=%llx\n",
                     (uint64_t)sp_array, sp_array_phys);
    }

    // Write DCBAA physical address to operational register
    op_regs->dcbaap = xhci_virt_to_phys(dcbaa);
    uart::printf("xhci: DCBAAP set to %llx\n", op_regs->dcbaap);
}

// NEW: Configure operational registers after reset
static void configure_operational_regs() {
    // Enable all device notifications
    op_regs->dnctrl = 0xFFFF;

    // Set max device slots
    op_regs->config = (uint32_t)max_device_slots;

    uart::printf("xhci: config=%u dnctrl=%x\n",
                 op_regs->config, op_regs->dnctrl);

    // Setup DCBAA and scratchpad
    setup_dcbaa();

    // Command ring setup will come in a later lesson
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

        configure_operational_regs();
        log_op_regs();

        return true;
    }
}