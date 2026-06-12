#include "../../../include/drivers/usb/xhci.h"

// Busy-wait delay
static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++)
        for (volatile uint32_t j = 0; j < 100000; j++)
            asm volatile("pause");
}

static void write_mmio64(volatile uint64_t* reg, uint64_t val) {
    volatile uint32_t* reg32 = (volatile uint32_t*)reg;
    reg32[0] = (uint32_t)(val & 0xFFFFFFFF);
    reg32[1] = (uint32_t)(val >> 32);
}

static uint64_t read_mmio64(volatile uint64_t* reg) {
    volatile uint32_t* reg32 = (volatile uint32_t*)reg;
    uint32_t lo = reg32[0];
    uint32_t hi = reg32[1];
    return ((uint64_t)hi << 32) | lo;
}

// Aligned memory allocator for xHCI structures
static void* alloc_xhci_memory(size_t size, size_t alignment, size_t boundary) {
    if (size == 0 || alignment == 0) {
        uart::printf("xhci: bad alloc params size=%u align=%u\n",
                     (uint32_t)size, (uint32_t)alignment);
        while (1) asm volatile("hlt");
    }

    size_t total = size + alignment + boundary + sizeof(void*);
    void* raw = kmalloc(total);
    if (!raw) {
        uart::printf("xhci: alloc failed size=%u\n", (uint32_t)size);
        while (1) asm volatile("hlt");
    }

    uintptr_t base = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (base + alignment - 1) & ~(alignment - 1);

    if (boundary > 0) {
        uintptr_t start_region = aligned / boundary;
        uintptr_t end_region = (aligned + size - 1) / boundary;
        if (start_region != end_region) {
            aligned = (end_region * boundary + alignment - 1) & ~(alignment - 1);
        }
    }

    ((void**)aligned)[-1] = raw;
    memory::memset((uint8_t*)aligned, 0, size);
    return (void*)aligned;
}

static void free_xhci_memory(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    kfree(raw);
}

static uintptr_t xhci_virt_to_phys(void* vaddr) {
    return paging::get_phys_addr((uint64_t)vaddr);
}

static uintptr_t xhci_map_mmio(uint64_t bar_addr, uint64_t bar_size) {
    return (uintptr_t)paging::map_mmio_region(bar_addr, bar_size);
}

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

// Command ring
struct xhci_cmd_ring {
    xhci_trb_t* trbs;
    uintptr_t   phys_base;
    size_t      max_trb_count;
    size_t      enqueue_ptr;
    uint8_t     cycle_bit;
};

static void cmd_ring_init(xhci_cmd_ring* ring, size_t max_trbs) {
    ring->max_trb_count = max_trbs;
    ring->cycle_bit = XHCI_CRCR_RING_CYCLE_STATE;
    ring->enqueue_ptr = 0;

    uint64_t ring_size = max_trbs * sizeof(xhci_trb_t);

    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(
        ring_size, XHCI_CMD_RING_ALIGNMENT, XHCI_CMD_RING_BOUNDARY);

    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->trbs[max_trbs - 1].parameter = ring->phys_base;
    ring->trbs[max_trbs - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;

    uart::printf("xhci: command ring virt=%llx phys=%llx trbs=%u\n",
                 (uint64_t)ring->trbs, (uint64_t)ring->phys_base, (uint32_t)max_trbs);
    uart::printf("xhci: link TRB[%u] -> phys=%llx control=%x\n",
                 (uint32_t)(max_trbs - 1),
                 ring->trbs[max_trbs - 1].parameter,
                 ring->trbs[max_trbs - 1].control);
}

static void cmd_ring_enqueue(xhci_cmd_ring* ring, xhci_trb_t* trb) {
    trb->cycle_bit = ring->cycle_bit;
    ring->trbs[ring->enqueue_ptr] = *trb;

    if (++ring->enqueue_ptr == ring->max_trb_count - 1) {
        ring->trbs[ring->max_trb_count - 1].control =
            (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;
        ring->enqueue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
}

// Event ring
struct xhci_evt_ring {
    xhci_trb_t*       trbs;
    uintptr_t          phys_base;
    xhci_erst_entry*   segment_table;
    volatile xhci_interrupter_regs* interrupter;
    size_t             segment_trb_count;
    uint64_t           dequeue_ptr;
    uint8_t            cycle_bit;
};

static void evt_ring_update_erdp(xhci_evt_ring* ring) {
    uint64_t dequeue_addr = ring->phys_base + (ring->dequeue_ptr * sizeof(xhci_trb_t));
    write_mmio64(&ring->interrupter->erdp, dequeue_addr);
}

static void evt_ring_init(xhci_evt_ring* ring, size_t max_trbs,
                           volatile xhci_interrupter_regs* interrupter) {
    ring->interrupter = interrupter;
    ring->segment_trb_count = max_trbs;
    ring->cycle_bit = XHCI_CRCR_RING_CYCLE_STATE;
    ring->dequeue_ptr = 0;

    uint64_t segment_size = max_trbs * sizeof(xhci_trb_t);
    uint64_t table_size = 1 * sizeof(xhci_erst_entry);

    // Allocate event ring segment
    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(
        segment_size, XHCI_EVT_RING_ALIGNMENT, XHCI_EVT_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    // Allocate segment table (single entry)
    ring->segment_table = (xhci_erst_entry*)alloc_xhci_memory(
        table_size, XHCI_ERST_ALIGNMENT, XHCI_ERST_BOUNDARY);

    // Fill segment table entry
    ring->segment_table[0].ring_segment_base_address = ring->phys_base;
    ring->segment_table[0].ring_segment_size = max_trbs;
    ring->segment_table[0].rsvd = 0;

    // Write ERSTSZ (number of segments = 1)
    interrupter->erstsz = 1;

    // Write ERDP first
    evt_ring_update_erdp(ring);

    // Write ERSTBA last (spec says writing ERSTBA may cause the controller
    // to read the segment table, so everything must be ready before this)
    write_mmio64(&interrupter->erstba, xhci_virt_to_phys(ring->segment_table));

    uart::printf("xhci: event ring virt=%llx phys=%llx trbs=%u\n",
                 (uint64_t)ring->trbs, (uint64_t)ring->phys_base, (uint32_t)max_trbs);
    uart::printf("xhci: ERST entry[0] base=%llx size=%u\n",
                 ring->segment_table[0].ring_segment_base_address,
                 ring->segment_table[0].ring_segment_size);
    uart::printf("xhci: ERSTSZ=%u ERSTBA=%llx ERDP=%llx\n",
                 interrupter->erstsz,
                 read_mmio64(&interrupter->erstba),
                 read_mmio64(&interrupter->erdp));
}

static bool evt_ring_has_events(xhci_evt_ring* ring) {
    return (ring->trbs[ring->dequeue_ptr].cycle_bit == ring->cycle_bit);
}

static xhci_trb_t* evt_ring_dequeue_trb(xhci_evt_ring* ring) {
    if (ring->trbs[ring->dequeue_ptr].cycle_bit != ring->cycle_bit) {
        uart::printf("xhci: event ring dequeue: no valid TRB\n");
        return nullptr;
    }

    xhci_trb_t* trb = &ring->trbs[ring->dequeue_ptr];

    if (++ring->dequeue_ptr == ring->segment_trb_count) {
        ring->dequeue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }

    return trb;
}

// Dequeue all pending events, update ERDP, clear EHB
static void evt_ring_flush(xhci_evt_ring* ring) {
    while (evt_ring_has_events(ring)) {
        evt_ring_dequeue_trb(ring);
    }
    evt_ring_update_erdp(ring);

    // Clear Event Handler Busy bit
    uint64_t erdp = read_mmio64(&ring->interrupter->erdp);
    erdp |= XHCI_ERDP_EHB;
    write_mmio64(&ring->interrupter->erdp, erdp);
}

// Driver state
static volatile xhci_cap_regs*      cap_regs     = nullptr;
static volatile xhci_op_regs*       op_regs      = nullptr;
static volatile xhci_runtime_regs*  runtime_regs = nullptr;
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

static uint64_t* dcbaa = nullptr;
static uint64_t* dcbaa_virt = nullptr;

static xhci_cmd_ring cmd_ring;
static xhci_evt_ring evt_ring;

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
    runtime_regs = (volatile xhci_runtime_regs*)(xhc_base + cap_regs->rtsoff);
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
    uart::printf("  crcr    : %llx\n", read_mmio64(&op_regs->crcr));
    uart::printf("  dcbaap  : %llx\n", read_mmio64(&op_regs->dcbaap));
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
        read_mmio64(&op_regs->crcr) != 0 ||
        read_mmio64(&op_regs->dcbaap) != 0 ||
        op_regs->config != 0) {
        uart::printf("xhci: unexpected register values after reset\n");
        return false;
    }

    uart::printf("xhci: controller reset successful\n");
    return true;
}

static void setup_dcbaa() {
    size_t dcbaa_size = sizeof(uint64_t) * (max_device_slots + 1);

    dcbaa = (uint64_t*)alloc_xhci_memory(dcbaa_size,
                                          XHCI_DCBAA_ALIGNMENT, XHCI_DCBAA_BOUNDARY);
    dcbaa_virt = (uint64_t*)kmalloc(sizeof(uint64_t) * (max_device_slots + 1));
    memory::memset((uint8_t*)dcbaa_virt, 0, sizeof(uint64_t) * (max_device_slots + 1));

    uart::printf("xhci: DCBAA virt=%llx phys=%llx slots=%u\n",
                 (uint64_t)dcbaa, (uint64_t)xhci_virt_to_phys(dcbaa),
                 (uint32_t)max_device_slots);

    if (max_scratchpad_bufs > 0) {
        uart::printf("xhci: allocating %u scratchpad buffers\n",
                     (uint32_t)max_scratchpad_bufs);

        uint64_t* sp_array = (uint64_t*)alloc_xhci_memory(
            max_scratchpad_bufs * sizeof(uint64_t),
            XHCI_DCBAA_ALIGNMENT, XHCI_DCBAA_BOUNDARY);

        for (uint32_t i = 0; i < max_scratchpad_bufs; i++) {
            void* sp_page = alloc_xhci_memory(4096,
                XHCI_SCRATCHPAD_BUF_ALIGNMENT, XHCI_SCRATCHPAD_BUF_BOUNDARY);
            sp_array[i] = xhci_virt_to_phys(sp_page);
            uart::printf("xhci: scratchpad[%u] virt=%llx phys=%llx\n",
                         i, (uint64_t)sp_page, sp_array[i]);
        }

        dcbaa[0] = xhci_virt_to_phys(sp_array);
        dcbaa_virt[0] = (uint64_t)sp_array;
        uart::printf("xhci: scratchpad array virt=%llx phys=%llx\n",
                     (uint64_t)sp_array, dcbaa[0]);
    }

    write_mmio64(&op_regs->dcbaap, xhci_virt_to_phys(dcbaa));
    uart::printf("xhci: DCBAAP set to %llx\n", read_mmio64(&op_regs->dcbaap));
}

static void configure_operational_regs() {
    op_regs->dnctrl = 0xFFFF;
    op_regs->config = (uint32_t)max_device_slots;

    uart::printf("xhci: config=%u dnctrl=%x\n", op_regs->config, op_regs->dnctrl);

    setup_dcbaa();

    cmd_ring_init(&cmd_ring, XHCI_COMMAND_RING_TRB_COUNT);

    uint64_t crcr_val = cmd_ring.phys_base | cmd_ring.cycle_bit;
    write_mmio64(&op_regs->crcr, crcr_val);
    uart::printf("xhci: CRCR written: %llx\n", crcr_val);
}

static void acknowledge_irq(uint8_t interrupter) {
    volatile xhci_interrupter_regs* ir = &runtime_regs->ir[interrupter];

    // Clear IP in IMAN (write-1-to-clear), preserve IE
    uint32_t iman = ir->iman;
    iman |= XHCI_IMAN_INTERRUPT_PENDING;
    ir->iman = iman;

    // Clear EINT in USBSTS (write-1-to-clear)
    op_regs->usbsts = XHCI_USBSTS_EINT;
}

static void configure_runtime_regs() {
    volatile xhci_interrupter_regs* ir = &runtime_regs->ir[0];

    // Enable interrupts on primary interrupter
    uint32_t iman = ir->iman;
    iman |= XHCI_IMAN_INTERRUPT_ENABLE;
    ir->iman = iman;

    // Setup event ring on primary interrupter
    evt_ring_init(&evt_ring, XHCI_EVENT_RING_TRB_COUNT, ir);

    uart::printf("xhci: runtime regs at %llx\n", (uint64_t)runtime_regs);
    uart::printf("xhci: interrupter[0] iman=%x imod=%x\n", ir->iman, ir->imod);

    // Clear any pending interrupts
    acknowledge_irq(0);

    uart::printf("xhci: primary interrupter configured\n");
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

        configure_runtime_regs();

        return true;
    }
}