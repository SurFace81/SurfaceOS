#include "../../../include/drivers/usb/xhci.h"

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

static const char* completion_code_str(uint8_t code) {
    switch (code) {
    case 0:  return "INVALID";
    case 1:  return "SUCCESS";
    case 2:  return "DATA_BUFFER_ERROR";
    case 3:  return "BABBLE_DETECTED";
    case 4:  return "USB_TRANSACTION_ERROR";
    case 5:  return "TRB_ERROR";
    case 6:  return "STALL_ERROR";
    case 7:  return "RESOURCE_ERROR";
    case 8:  return "BANDWIDTH_ERROR";
    case 9:  return "NO_SLOTS_AVAILABLE";
    case 13: return "SHORT_PACKET";
    case 17: return "PARAMETER_ERROR";
    case 19: return "CONTEXT_STATE_ERROR";
    case 21: return "EVENT_RING_FULL";
    case 24: return "COMMAND_RING_STOPPED";
    case 25: return "COMMAND_ABORTED";
    default: return "UNKNOWN";
    }
}

static const char* trb_type_str(uint8_t type) {
    switch (type) {
    case 0:  return "RESERVED";
    case 1:  return "NORMAL";
    case 6:  return "LINK";
    case 8:  return "NOOP";
    case 9:  return "ENABLE_SLOT_CMD";
    case 10: return "DISABLE_SLOT_CMD";
    case 11: return "ADDRESS_DEVICE_CMD";
    case 12: return "CONFIGURE_ENDPOINT_CMD";
    case 23: return "NOOP_CMD";
    case 32: return "TRANSFER_EVENT";
    case 33: return "CMD_COMPLETION_EVENT";
    case 34: return "PORT_STATUS_CHANGE_EVENT";
    case 37: return "HOST_CONTROLLER_EVENT";
    default: return "UNKNOWN";
    }
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

    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(
        max_trbs * sizeof(xhci_trb_t), XHCI_CMD_RING_ALIGNMENT, XHCI_CMD_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->trbs[max_trbs - 1].parameter = ring->phys_base;
    ring->trbs[max_trbs - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;

    uart::printf("xhci: command ring virt=%llx phys=%llx trbs=%u\n",
                 (uint64_t)ring->trbs, (uint64_t)ring->phys_base, (uint32_t)max_trbs);
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
    uint64_t addr = ring->phys_base + (ring->dequeue_ptr * sizeof(xhci_trb_t));
    write_mmio64(&ring->interrupter->erdp, addr);
}

static void evt_ring_init(xhci_evt_ring* ring, size_t max_trbs,
                           volatile xhci_interrupter_regs* interrupter) {
    ring->interrupter = interrupter;
    ring->segment_trb_count = max_trbs;
    ring->cycle_bit = XHCI_CRCR_RING_CYCLE_STATE;
    ring->dequeue_ptr = 0;

    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(
        max_trbs * sizeof(xhci_trb_t), XHCI_EVT_RING_ALIGNMENT, XHCI_EVT_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->segment_table = (xhci_erst_entry*)alloc_xhci_memory(
        sizeof(xhci_erst_entry), XHCI_ERST_ALIGNMENT, XHCI_ERST_BOUNDARY);

    ring->segment_table[0].ring_segment_base_address = ring->phys_base;
    ring->segment_table[0].ring_segment_size = max_trbs;
    ring->segment_table[0].rsvd = 0;

    interrupter->erstsz = 1;
    evt_ring_update_erdp(ring);
    write_mmio64(&interrupter->erstba, xhci_virt_to_phys(ring->segment_table));

    uart::printf("xhci: event ring virt=%llx phys=%llx trbs=%u\n",
                 (uint64_t)ring->trbs, (uint64_t)ring->phys_base, (uint32_t)max_trbs);
}

static bool evt_ring_has_events(xhci_evt_ring* ring) {
    return (ring->trbs[ring->dequeue_ptr].cycle_bit == ring->cycle_bit);
}

static xhci_trb_t* evt_ring_dequeue_trb(xhci_evt_ring* ring) {
    if (ring->trbs[ring->dequeue_ptr].cycle_bit != ring->cycle_bit)
        return nullptr;

    xhci_trb_t* trb = &ring->trbs[ring->dequeue_ptr];
    if (++ring->dequeue_ptr == ring->segment_trb_count) {
        ring->dequeue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
    return trb;
}

static void evt_ring_flush(xhci_evt_ring* ring) {
    while (evt_ring_has_events(ring))
        evt_ring_dequeue_trb(ring);
    evt_ring_update_erdp(ring);
    uint64_t erdp = read_mmio64(&ring->interrupter->erdp);
    erdp |= XHCI_ERDP_EHB;
    write_mmio64(&ring->interrupter->erdp, erdp);
}

// Doorbell
static volatile xhci_doorbell_reg* doorbells = nullptr;

static void ring_doorbell(uint8_t slot, uint8_t target) {
    doorbells[slot].raw = (uint32_t)target;
}

static void ring_command_doorbell() {
    ring_doorbell(0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

// Transfer ring (same structure as command ring, but per-endpoint)
struct xhci_transfer_ring {
    xhci_trb_t* trbs;
    uintptr_t   phys_base;
    size_t      max_trb_count;
    size_t      enqueue_ptr;
    uint8_t     cycle_bit;
};

static void transfer_ring_init(xhci_transfer_ring* ring, size_t max_trbs) {
    ring->max_trb_count = max_trbs;
    ring->cycle_bit = 1;
    ring->enqueue_ptr = 0;

    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(
        max_trbs * sizeof(xhci_trb_t),
        XHCI_TRANSFER_RING_ALIGNMENT,
        XHCI_TRANSFER_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    // Link TRB at the end, pointing back to start
    ring->trbs[max_trbs - 1].parameter = ring->phys_base;
    ring->trbs[max_trbs - 1].status = 0;
    ring->trbs[max_trbs - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;

    uart::printf("xhci: transfer ring virt=%llx phys=%llx\n",
                 (uint64_t)ring->trbs, (uint64_t)ring->phys_base);
}

static void transfer_ring_enqueue(xhci_transfer_ring* ring, xhci_trb_t* trb) {
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
static bool     ac64, csz, ppc, pind, lhrc;
static uint32_t xecp_offset;

static uint64_t* dcbaa = nullptr;
static uint64_t* dcbaa_virt = nullptr;

static xhci_transfer_ring ep0_rings[65]; // indexed by slot_id (1-based, max 64 slots)

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
    ac64  = XHCI_AC64(cap_regs);
    csz   = XHCI_CSZ(cap_regs);
    ppc   = XHCI_PPC(cap_regs);
    pind  = XHCI_PIND(cap_regs);
    lhrc  = XHCI_LHRC(cap_regs);
    xecp_offset = XHCI_XECP(cap_regs) * sizeof(uint32_t);

    op_regs = (volatile xhci_op_regs*)(xhc_base + cap_regs->caplength);
    runtime_regs = (volatile xhci_runtime_regs*)(xhc_base + cap_regs->rtsoff);
    doorbells = (volatile xhci_doorbell_reg*)(xhc_base + cap_regs->dboff);
}

static void log_cap_regs() {
    uart::printf("xHCI Capability Registers:\n");
    uart::printf("  caplength       : %u\n",  (uint32_t)cap_regs->caplength);
    uart::printf("  hciversion      : %x\n",  (uint32_t)cap_regs->hciversion);
    uart::printf("  max_device_slots: %u\n",  (uint32_t)max_device_slots);
    uart::printf("  max_interrupters: %u\n",  (uint32_t)max_interrupters_val);
    uart::printf("  max_ports       : %u\n",  (uint32_t)max_ports);
    uart::printf("  scratchpad bufs : %u\n",  (uint32_t)max_scratchpad_bufs);
    uart::printf("  64-bit addr     : %s\n",  ac64 ? "yes" : "no");
    uart::printf("  xECP offset     : %x\n",  xecp_offset);
}

static void log_op_regs() {
    uart::printf("xHCI Operational Registers:\n");
    uart::printf("  usbcmd  : %x\n",  op_regs->usbcmd);
    uart::printf("  usbsts  : %x\n",  op_regs->usbsts);
    uart::printf("  pagesize: %x\n",  op_regs->pagesize);
    uart::printf("  config  : %x\n",  op_regs->config);
}

static void log_usbsts() {
    uint32_t s = op_regs->usbsts;
    uart::printf("xhci: USBSTS = %x\n", s);
    if (s & XHCI_USBSTS_HCH)  uart::printf("  HCH\n");
    if (s & XHCI_USBSTS_HSE)  uart::printf("  HSE\n");
    if (s & XHCI_USBSTS_EINT) uart::printf("  EINT\n");
    if (s & XHCI_USBSTS_PCD)  uart::printf("  PCD\n");
    if (s & XHCI_USBSTS_CNR)  uart::printf("  CNR\n");
    if (s & XHCI_USBSTS_HCE)  uart::printf("  HCE\n");
}

static bool take_ownership_from_bios() {
    if (xecp_offset == 0) return true;
    volatile uint32_t* ecap = (volatile uint32_t*)(xhc_base + xecp_offset);
    while (true) {
        uint32_t val = *ecap;
        uint8_t cap_id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;
        if (cap_id == XHCI_LEGACY_SUPPORT_CAP_ID) {
            *ecap = val | XHCI_LEGACY_OS_OWNED;
            uint32_t timeout = 500;
            while ((*ecap & XHCI_LEGACY_BIOS_OWNED) && timeout > 0) { delay_ms(1); timeout--; }
            if (*ecap & XHCI_LEGACY_BIOS_OWNED) {
                uart::printf("xhci: BIOS did not release ownership\n");
                return false;
            }
            volatile uint32_t* leg_ctrl = ecap + 1;
            *leg_ctrl &= ~(uint32_t)0x0000E01F;
            uart::printf("xhci: took ownership from BIOS\n");
            return true;
        }
        if (next == 0) break;
        ecap = XHCI_NEXT_EXT_CAP_PTR(ecap, next);
    }
    return true;
}

static bool reset_controller() {
    op_regs->usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    uint32_t timeout = 200;
    while (!(op_regs->usbsts & XHCI_USBSTS_HCH)) { if (--timeout == 0) return false; delay_ms(1); }

    op_regs->usbcmd |= XHCI_USBCMD_HCRESET;
    timeout = 1000;
    while ((op_regs->usbcmd & XHCI_USBCMD_HCRESET) || (op_regs->usbsts & XHCI_USBSTS_CNR)) {
        if (--timeout == 0) return false; delay_ms(1);
    }
    delay_ms(50);
    uart::printf("xhci: controller reset successful\n");
    return true;
}

static void setup_dcbaa() {
    size_t dcbaa_size = sizeof(uint64_t) * (max_device_slots + 1);
    dcbaa = (uint64_t*)alloc_xhci_memory(dcbaa_size, XHCI_DCBAA_ALIGNMENT, XHCI_DCBAA_BOUNDARY);
    dcbaa_virt = (uint64_t*)kmalloc(sizeof(uint64_t) * (max_device_slots + 1));
    memory::memset((uint8_t*)dcbaa_virt, 0, sizeof(uint64_t) * (max_device_slots + 1));

    if (max_scratchpad_bufs > 0) {
        uart::printf("xhci: allocating %u scratchpad buffers\n", (uint32_t)max_scratchpad_bufs);
        uint64_t* sp_array = (uint64_t*)alloc_xhci_memory(
            max_scratchpad_bufs * sizeof(uint64_t), XHCI_DCBAA_ALIGNMENT, XHCI_DCBAA_BOUNDARY);
        for (uint32_t i = 0; i < max_scratchpad_bufs; i++) {
            void* sp_page = alloc_xhci_memory(4096, XHCI_SCRATCHPAD_BUF_ALIGNMENT, XHCI_SCRATCHPAD_BUF_BOUNDARY);
            sp_array[i] = xhci_virt_to_phys(sp_page);
        }
        dcbaa[0] = xhci_virt_to_phys(sp_array);
        dcbaa_virt[0] = (uint64_t)sp_array;
    }

    write_mmio64(&op_regs->dcbaap, xhci_virt_to_phys(dcbaa));
    uart::printf("xhci: DCBAAP set to %llx\n", (uint64_t)xhci_virt_to_phys(dcbaa));
}

static void configure_operational_regs() {
    op_regs->dnctrl = 0xFFFF;
    op_regs->config = (uint32_t)max_device_slots;
    setup_dcbaa();
    cmd_ring_init(&cmd_ring, XHCI_COMMAND_RING_TRB_COUNT);
    write_mmio64(&op_regs->crcr, cmd_ring.phys_base | cmd_ring.cycle_bit);
}

static void acknowledge_irq(uint8_t interrupter) {
    volatile xhci_interrupter_regs* ir = &runtime_regs->ir[interrupter];
    uint32_t iman = ir->iman;
    iman |= XHCI_IMAN_INTERRUPT_PENDING;
    ir->iman = iman;
    op_regs->usbsts = XHCI_USBSTS_EINT;
}

static void configure_runtime_regs() {
    volatile xhci_interrupter_regs* ir = &runtime_regs->ir[0];
    ir->iman |= XHCI_IMAN_INTERRUPT_ENABLE;
    evt_ring_init(&evt_ring, XHCI_EVENT_RING_TRB_COUNT, ir);
    acknowledge_irq(0);
    uart::printf("xhci: primary interrupter configured\n");
}

static bool start_controller() {
    uint32_t usbcmd = op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_RUN_STOP | XHCI_USBCMD_INTERRUPTER_ENABLE | XHCI_USBCMD_HOSTSYS_ERR_EN;
    op_regs->usbcmd = usbcmd;

    uint32_t timeout = 1000;
    while (op_regs->usbsts & XHCI_USBSTS_HCH) { if (--timeout == 0) return false; delay_ms(1); }
    if (op_regs->usbsts & XHCI_USBSTS_CNR) return false;

    uart::printf("xhci: controller started\n");
    return true;
}

// Process all pending events from event ring (polling mode)
static void process_events() {
    while (evt_ring_has_events(&evt_ring)) {
        xhci_trb_t* trb = evt_ring_dequeue_trb(&evt_ring);
        if (!trb) break;

        uart::printf("xhci: event type=%u (%s)\n",
                     (uint32_t)trb->trb_type, trb_type_str(trb->trb_type));

        if (trb->trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
            xhci_cmd_completion_trb_t* cc = (xhci_cmd_completion_trb_t*)trb;
            uart::printf("xhci:   completion_code=%u (%s) slot_id=%u\n",
                         (uint32_t)cc->completion_code,
                         completion_code_str(cc->completion_code),
                         (uint32_t)cc->slot_id);
        }
    }

    evt_ring_update_erdp(&evt_ring);
    uint64_t erdp = read_mmio64(&evt_ring.interrupter->erdp);
    erdp |= XHCI_ERDP_EHB;
    write_mmio64(&evt_ring.interrupter->erdp, erdp);
    acknowledge_irq(0);
}

// Send a command TRB and poll for completion
static xhci_cmd_completion_trb_t* send_command(xhci_trb_t* cmd_trb, uint32_t timeout_ms) {
    // Flush any pending events first
    process_events();

    cmd_ring_enqueue(&cmd_ring, cmd_trb);
    ring_command_doorbell();

    // Poll for completion
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (evt_ring_has_events(&evt_ring)) {
            xhci_trb_t* trb = evt_ring_dequeue_trb(&evt_ring);
            if (!trb) break;

            uart::printf("xhci: event type=%u (%s)\n",
                         (uint32_t)trb->trb_type, trb_type_str(trb->trb_type));

            if (trb->trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                xhci_cmd_completion_trb_t* cc = (xhci_cmd_completion_trb_t*)trb;

                // Update ERDP and clear EHB
                evt_ring_update_erdp(&evt_ring);
                uint64_t erdp = read_mmio64(&evt_ring.interrupter->erdp);
                erdp |= XHCI_ERDP_EHB;
                write_mmio64(&evt_ring.interrupter->erdp, erdp);
                acknowledge_irq(0);

                return cc;
            }
            // Non-completion event, keep polling
        }
        delay_ms(1);
        elapsed++;
    }

    uart::printf("xhci: command timeout after %u ms\n", timeout_ms);
    return nullptr;
}

// Extended capability: Supported Protocol (xHCI spec section 7.2)
struct xhci_supported_protocol {
    uint8_t id;
    uint8_t next;
    uint8_t minor_rev;
    uint8_t major_rev;
    uint32_t name;
    uint8_t compatible_port_offset;
    uint8_t compatible_port_count;
    uint8_t protocol_defined;
    uint8_t psic;
    uint32_t dword3;
};

static void read_supported_protocol(volatile uint32_t* cap, xhci_supported_protocol* out) {
    uint32_t dw0 = cap[0];
    out->id        = dw0 & 0xFF;
    out->next      = (dw0 >> 8) & 0xFF;
    out->minor_rev = (dw0 >> 16) & 0xFF;
    out->major_rev = (dw0 >> 24) & 0xFF;
    out->name      = cap[1];
    uint32_t dw2   = cap[2];
    out->compatible_port_offset = dw2 & 0xFF;
    out->compatible_port_count  = (dw2 >> 8) & 0xFF;
    out->protocol_defined       = (dw2 >> 16) & 0xFF;
    out->psic                   = (dw2 >> 24) & 0xFF;
    out->dword3    = cap[3];
}

#define XHCI_EXT_CAP_LEGACY        1
#define XHCI_EXT_CAP_PROTOCOL      2

#define XHCI_MAX_USB3_PORTS 32

static uint8_t usb3_ports[XHCI_MAX_USB3_PORTS];
static uint8_t usb3_port_count = 0;

static void parse_extended_capabilities() {
    if (xecp_offset == 0) return;

    volatile uint32_t* cap = (volatile uint32_t*)(xhc_base + xecp_offset);
    usb3_port_count = 0;

    while (true) {
        uint32_t dw0 = *cap;
        uint8_t cap_id = dw0 & 0xFF;
        uint8_t next = (dw0 >> 8) & 0xFF;

        if (cap_id == XHCI_EXT_CAP_PROTOCOL) {
            xhci_supported_protocol proto;
            read_supported_protocol(cap, &proto);

            // port_offset is 1-based in the spec, convert to 0-based
            uint8_t first_port = proto.compatible_port_offset - 1;
            uint8_t port_count = proto.compatible_port_count;

            char name_str[5];
            *((uint32_t*)name_str) = proto.name;
            name_str[4] = '\0';

            uart::printf("xhci: protocol '%s' rev=%u.%u ports %u-%u\n",
                         name_str,
                         (uint32_t)proto.major_rev, (uint32_t)proto.minor_rev,
                         (uint32_t)first_port, (uint32_t)(first_port + port_count - 1));

            if (proto.major_rev == 3) {
                for (uint8_t i = 0; i < port_count && usb3_port_count < XHCI_MAX_USB3_PORTS; i++) {
                    usb3_ports[usb3_port_count++] = first_port + i;
                }
            }
        }

        if (next == 0) break;
        cap = (volatile uint32_t*)((char*)cap + (next * sizeof(uint32_t)));
    }

    uart::printf("xhci: %u USB3 ports found\n", (uint32_t)usb3_port_count);
}

static bool is_usb3_port(uint8_t port_num) {
    for (uint8_t i = 0; i < usb3_port_count; i++) {
        if (usb3_ports[i] == port_num) return true;
    }
    return false;
}

static xhci_portsc read_portsc(uint8_t port) {
    uint64_t addr = (uint64_t)op_regs + 0x400 + (0x10 * port);
    xhci_portsc reg;
    reg.raw = *(volatile uint32_t*)addr;
    return reg;
}

static void write_portsc(xhci_portsc reg, uint8_t port) {
    uint64_t addr = (uint64_t)op_regs + 0x400 + (0x10 * port);
    *(volatile uint32_t*)addr = reg.raw;
}

static const char* usb_speed_str(uint8_t speed) {
    switch (speed) {
    case 1: return "Full Speed (12 Mb/s)";
    case 2: return "Low Speed (1.5 Mb/s)";
    case 3: return "High Speed (480 Mb/s)";
    case 4: return "SuperSpeed (5 Gb/s)";
    case 5: return "SuperSpeed+ (10 Gb/s)";
    default: return "Unknown";
    }
}

static bool reset_port(uint8_t port_num) {
    xhci_portsc portsc = read_portsc(port_num);
    bool usb3 = is_usb3_port(port_num);

    // Power on if needed
    if (portsc.pp == 0) {
        portsc.pp = 1;
        write_portsc(portsc, port_num);
        delay_ms(20);
        portsc = read_portsc(port_num);
        if (portsc.pp == 0) {
            uart::printf("xhci: port %u failed to power on\n", (uint32_t)port_num);
            return false;
        }
    }

    // Clear lingering status change bits
    portsc.csc = 1;
    portsc.pec = 1;
    portsc.prc = 1;
    write_portsc(portsc, port_num);

    // Initiate reset
    portsc = read_portsc(port_num);
    if (usb3) {
        portsc.wpr = 1; // Warm reset for USB3
    } else {
        portsc.pr = 1;  // Standard reset for USB2
    }
    write_portsc(portsc, port_num);

    // Wait for reset completion
    uint32_t timeout = 100;
    while (timeout > 0) {
        portsc = read_portsc(port_num);
        if ((usb3 && portsc.wrc) || (!usb3 && portsc.prc)) {
            break;
        }
        timeout--;
        delay_ms(1);
    }

    if (timeout == 0) {
        uart::printf("xhci: port %u reset timed out\n", (uint32_t)port_num);
        return false;
    }

    delay_ms(3);

    // Clear reset completion bits (write-1-to-clear), preserve PED
    portsc = read_portsc(port_num);
    portsc.prc = 1;
    portsc.wrc = 1;
    portsc.csc = 1;
    portsc.pec = 1;
    portsc.ped = 0; // Don't accidentally clear PED
    write_portsc(portsc, port_num);

    delay_ms(3);

    // Verify port is enabled
    portsc = read_portsc(port_num);
    if (portsc.ped == 0) {
        uart::printf("xhci: port %u not enabled after reset\n", (uint32_t)port_num);
        return false;
    }

    return true;
}

// Enable a device slot, returns slot_id or 0 on failure
static uint8_t enable_device_slot() {
    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.trb_type = XHCI_TRB_TYPE_ENABLE_SLOT_CMD;

    xhci_cmd_completion_trb_t* cc = send_command(&trb, 200);
    if (!cc) return 0;

    uart::printf("xhci: enable slot: code=%u slot_id=%u\n",
                 (uint32_t)cc->completion_code, (uint32_t)cc->slot_id);

    if (cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
        return 0;

    return cc->slot_id;
}

// Create output device context for a slot and register in DCBAA
static bool create_device_context(uint8_t slot_id) {
    void* ctx = alloc_xhci_memory(sizeof(xhci_device_context),
                                   XHCI_DEVICE_CTX_ALIGNMENT,
                                   XHCI_DEVICE_CTX_BOUNDARY);
    if (!ctx) {
        uart::printf("xhci: failed to alloc device context\n");
        return false;
    }

    dcbaa[slot_id] = xhci_virt_to_phys(ctx);
    dcbaa_virt[slot_id] = (uint64_t)ctx;

    uart::printf("xhci: device context[%u] virt=%llx phys=%llx\n",
                 (uint32_t)slot_id, (uint64_t)ctx, dcbaa[slot_id]);
    return true;
}

// Allocate input context for a device
static xhci_input_context* alloc_input_context() {
    xhci_input_context* ctx = (xhci_input_context*)alloc_xhci_memory(
        sizeof(xhci_input_context),
        XHCI_INPUT_CTX_ALIGNMENT,
        XHCI_INPUT_CTX_BOUNDARY);
    return ctx;
}

static uint16_t max_packet_size_for_speed(uint8_t speed) {
    switch (speed) {
    case 1: return 64;   // Full Speed
    case 2: return 8;    // Low Speed
    case 3: return 64;   // High Speed
    case 4: return 512;  // SuperSpeed
    case 5: return 512;  // SuperSpeed+
    default: return 64;
    }
}

static void setup_device(uint8_t port_index) {
    xhci_portsc portsc = read_portsc(port_index);
    uint8_t port_speed = portsc.port_speed;
    uint8_t port_id = port_index + 1; // 1-based for slot context

    // Step 1: Enable a device slot
    uint8_t slot_id = enable_device_slot();
    if (slot_id == 0) {
        uart::printf("xhci: failed to enable slot for port %u\n", (uint32_t)port_index);
        return;
    }

    // Step 2: Create output device context and register in DCBAA
    if (!create_device_context(slot_id)) {
        uart::printf("xhci: failed to create device context for slot %u\n", (uint32_t)slot_id);
        return;
    }

    // Step 3: Allocate transfer ring for EP0
    xhci_transfer_ring* ep0_ring = &ep0_rings[slot_id];
    transfer_ring_init(ep0_ring, XHCI_TRANSFER_RING_TRB_COUNT);

    // Step 4: Build Input Context
    xhci_input_context* input_ctx = alloc_input_context();
    if (!input_ctx) {
        uart::printf("xhci: failed to alloc input context\n");
        return;
    }

    // Input Control Context: add Slot (bit 0) and EP0 (bit 1)
    input_ctx->control_context.add_flags = (1 << 0) | (1 << 1);
    input_ctx->control_context.drop_flags = 0;

    // Slot Context
    xhci_slot_context* slot = &input_ctx->device_context.slot_context;
    slot->route_string = 0;
    slot->speed = port_speed;
    slot->context_entries = 1; // only EP0 is active
    slot->root_hub_port_num = port_id;

    // Endpoint 0 Context (index 0 in device_context = control_ep_context)
    xhci_endpoint_context* ep0 = &input_ctx->device_context.control_ep_context;
    ep0->endpoint_type = XHCI_EP_TYPE_CONTROL_BIDIR; // 4
    ep0->max_packet_size = max_packet_size_for_speed(port_speed);
    ep0->max_burst_size = 0;
    ep0->error_count = 3;
    ep0->average_trb_length = 8;
    // Dequeue pointer: physical address of transfer ring with DCS=1
    ep0->transfer_ring_dequeue_ptr = ep0_ring->phys_base | 1;

    uart::printf("xhci: address device:\n");
    uart::printf("  port      : %u (1-based: %u)\n", (uint32_t)port_index, (uint32_t)port_id);
    uart::printf("  slot      : %u\n", (uint32_t)slot_id);
    uart::printf("  speed     : %u (%s)\n", (uint32_t)port_speed, usb_speed_str(port_speed));
    uart::printf("  max_pkt   : %u\n", (uint32_t)ep0->max_packet_size);
    uart::printf("  ep0 ring  : phys=%llx\n", (uint64_t)ep0_ring->phys_base);
    uart::printf("  input_ctx : phys=%llx\n", (uint64_t)xhci_virt_to_phys(input_ctx));
    uart::printf("  output_ctx: phys=%llx\n", dcbaa[slot_id]);

    // Step 5: Send Address Device Command (TRB type 11)
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.status = 0;
    cmd.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(&cmd, 500);
    if (!cc) {
        uart::printf("xhci: address device command timeout\n");
        return;
    }

    uart::printf("xhci: address device result: code=%u (%s) slot=%u\n",
                 (uint32_t)cc->completion_code,
                 completion_code_str(cc->completion_code),
                 (uint32_t)cc->slot_id);

    if (cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: address device FAILED\n");
        return;
    }

    // Verify: read back assigned USB address from output device context
    xhci_device_context* out_ctx = (xhci_device_context*)dcbaa_virt[slot_id];
    uart::printf("xhci: device addressed! usb_addr=%u slot_state=%u\n",
                 (uint32_t)out_ctx->slot_context.device_address,
                 (uint32_t)out_ctx->slot_context.slot_state);
}

namespace xhci {
    bool init() {
        PCIDevice* dev = pci::find(PCI_CLASS_SERIAL, 0x03, 0x30);
        if (!dev) { uart::printf("xhci: no xHCI controller found\n"); return false; }

        uart::printf("xhci: found controller %x:%x at %u:%u.%u\n",
            (uint32_t)dev->vendor_id, (uint32_t)dev->device_id,
            (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);

        pci::enable_device(dev);

        PCIBar bar = pci::get_bar(dev, 0);
        if (!bar.valid || bar.is_io) { uart::printf("xhci: invalid BAR0\n"); return false; }

        uint64_t bar_size = get_bar_size_64(dev, 0);
        uart::printf("xhci: BAR0 phys=%llx size=%llx\n", bar.base, bar_size);

        xhc_base = xhci_map_mmio(bar.base, bar_size);
        parse_cap_regs();
        log_cap_regs();
        parse_extended_capabilities();

        if (!take_ownership_from_bios()) return false;
        if (!reset_controller()) return false;

        configure_operational_regs();
        configure_runtime_regs();

        if (!start_controller()) return false;

        // Flush pending port status change events
        process_events();

        // Scan ports, reset connected devices, setup device contexts
        for (uint8_t i = 0; i < max_ports; i++) {
            xhci_portsc portsc = read_portsc(i);

            if (portsc.ccs) {
                uart::printf("xhci: port %u: device detected (USB%u, speed=%u)\n",
                             (uint32_t)i, is_usb3_port(i) ? 3 : 2,
                             (uint32_t)portsc.port_speed);

                if (reset_port(i)) {
                    portsc = read_portsc(i);
                    uart::printf("xhci: port %u: reset OK, %s\n",
                                 (uint32_t)i, usb_speed_str(portsc.port_speed));
                    process_events();
                    setup_device(i);
                } else {
                    uart::printf("xhci: port %u: reset failed\n", (uint32_t)i);
                }
            }
        }

        return true;
    }
}