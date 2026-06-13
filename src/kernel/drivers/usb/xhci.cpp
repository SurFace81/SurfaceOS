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

// Per-device info we need for later steps
struct usb_mass_storage_dev {
    uint8_t slot_id;
    uint8_t port_index;
    uint8_t port_speed;
    uint8_t config_value;
    uint8_t interface_number;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    xhci_transfer_ring bulk_in_ring;
    xhci_transfer_ring bulk_out_ring;
    bool configured;
    bool found;
};

#define MAX_MASS_STORAGE_DEVS 8
static usb_mass_storage_dev mass_storage_devs[MAX_MASS_STORAGE_DEVS];
static uint8_t mass_storage_count = 0;

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

static xhci_transfer_event_trb_t* wait_transfer_event(uint32_t timeout_ms) {
    xhci_transfer_event_trb_t* last_transfer = nullptr;
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        if (evt_ring_has_events(&evt_ring)) {
            xhci_trb_t* trb = evt_ring_dequeue_trb(&evt_ring);
            if (!trb) break;

            uart::printf("xhci: [wait] event type=%u (%s)\n",
                         (uint32_t)trb->trb_type, trb_type_str(trb->trb_type));

            if (trb->trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                xhci_transfer_event_trb_t* te = (xhci_transfer_event_trb_t*)trb;
                uart::printf("xhci: [wait]   code=%u (%s) slot=%u ep=%u residue=%u trb_ptr=%llx\n",
                             (uint32_t)te->completion_code,
                             completion_code_str(te->completion_code),
                             (uint32_t)te->slot_id,
                             (uint32_t)te->endpoint_id,
                             (uint32_t)te->transfer_length,
                             te->trb_pointer);
                last_transfer = te;

                // If success or short packet on non-final TRB, keep reading events
                // If stall or error, return immediately
                if (te->completion_code != XHCI_TRB_COMPLETION_SUCCESS &&
                    te->completion_code != 13) {
                    // Error — return it
                    evt_ring_update_erdp(&evt_ring);
                    uint64_t erdp = read_mmio64(&evt_ring.interrupter->erdp);
                    erdp |= XHCI_ERDP_EHB;
                    write_mmio64(&evt_ring.interrupter->erdp, erdp);
                    acknowledge_irq(0);
                    return te;
                }

                // Success or short packet — check if this is the Status Stage (final)
                // IOC is on Status Stage, so SUCCESS on it means we're done
                if (te->completion_code == XHCI_TRB_COMPLETION_SUCCESS) {
                    evt_ring_update_erdp(&evt_ring);
                    uint64_t erdp = read_mmio64(&evt_ring.interrupter->erdp);
                    erdp |= XHCI_ERDP_EHB;
                    write_mmio64(&evt_ring.interrupter->erdp, erdp);
                    acknowledge_irq(0);
                    return te;
                }

                // Short packet — there might be another event for Status Stage
                // Continue polling
            }
        }
        delay_ms(1);
        elapsed++;
    }

    if (last_transfer) {
        evt_ring_update_erdp(&evt_ring);
        uint64_t erdp = read_mmio64(&evt_ring.interrupter->erdp);
        erdp |= XHCI_ERDP_EHB;
        write_mmio64(&evt_ring.interrupter->erdp, erdp);
        acknowledge_irq(0);
        return last_transfer;
    }

    uart::printf("xhci: transfer event timeout after %u ms\n", timeout_ms);
    return nullptr;
}

// Perform a control IN transfer (read data from device via EP0)
// setup_packet: 8 bytes of USB setup packet
// buffer: destination for data (physically contiguous)
// buffer_phys: physical address of buffer
// data_length: how many bytes to read
// Returns actual bytes transferred, or -1 on error
static sint32_t control_transfer_in(uint8_t slot_id, uint8_t* setup_packet,
                                     void* buffer, uintptr_t buffer_phys,
                                     uint16_t data_length) {
    xhci_transfer_ring* ring = &ep0_rings[slot_id];

    // Setup Stage TRB (type 2)
    xhci_trb_t setup_trb;
    memory::memset((uint8_t*)&setup_trb, 0, sizeof(xhci_trb_t));
    // Pack setup packet directly into parameter as uint64_t (little-endian)
    setup_trb.parameter = (uint64_t)setup_packet[0]
                        | ((uint64_t)setup_packet[1] << 8)
                        | ((uint64_t)setup_packet[2] << 16)
                        | ((uint64_t)setup_packet[3] << 24)
                        | ((uint64_t)setup_packet[4] << 32)
                        | ((uint64_t)setup_packet[5] << 40)
                        | ((uint64_t)setup_packet[6] << 48)
                        | ((uint64_t)setup_packet[7] << 56);
    setup_trb.status = 8; // transfer length = 8
    // TRB Type=2 (bits 10-15), IDT=1 (bit 6), TRT=3 (bits 16-17)
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
                      | (1 << 6)
                      | (3 << 16);
    transfer_ring_enqueue(ring, &setup_trb);

    // Data Stage TRB (type 3)
    xhci_trb_t data_trb;
    memory::memset((uint8_t*)&data_trb, 0, sizeof(xhci_trb_t));
    data_trb.parameter = (uint64_t)buffer_phys;
    data_trb.status = data_length;
    data_trb.control = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 16);  // DIR = 1 (IN)
    transfer_ring_enqueue(ring, &data_trb);

    // Status Stage TRB (type 4)
    // Direction is opposite to data: data=IN, so status=OUT (dir=0)
    xhci_trb_t status_trb;
    memory::memset((uint8_t*)&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT)
                       | (1 << 5); // IOC (Interrupt On Completion)
    transfer_ring_enqueue(ring, &status_trb);

    // Ring doorbell for this slot, target = 1 (Control EP, DCI=1)
    ring_doorbell(slot_id, XHCI_DOORBELL_TARGET_CONTROL_EP);

    // Wait for completion
    xhci_transfer_event_trb_t* evt = wait_transfer_event(500);
    if (!evt) {
        uart::printf("xhci: control transfer timeout slot=%u\n", (uint32_t)slot_id);
        return -1;
    }

    if (evt->completion_code != XHCI_TRB_COMPLETION_SUCCESS &&
        evt->completion_code != 13 /* SHORT_PACKET */) {
        uart::printf("xhci: control transfer failed slot=%u code=%u (%s)\n",
                     (uint32_t)slot_id,
                     (uint32_t)evt->completion_code,
                     completion_code_str(evt->completion_code));
        return -1;
    }

    // Actual bytes transferred = requested - residue
    sint32_t transferred = (sint32_t)data_length - (sint32_t)evt->transfer_length;
    return transferred;
}

// Control transfer with no data stage (e.g. SET_CONFIGURATION, SET_INTERFACE)
static bool control_transfer_no_data(uint8_t slot_id, uint8_t* setup_packet) {
    xhci_transfer_ring* ring = &ep0_rings[slot_id];

    // Setup Stage TRB (type 2), TRT=0 (No Data Stage)
    xhci_trb_t setup_trb;
    memory::memset((uint8_t*)&setup_trb, 0, sizeof(xhci_trb_t));
    setup_trb.parameter = (uint64_t)setup_packet[0]
                        | ((uint64_t)setup_packet[1] << 8)
                        | ((uint64_t)setup_packet[2] << 16)
                        | ((uint64_t)setup_packet[3] << 24)
                        | ((uint64_t)setup_packet[4] << 32)
                        | ((uint64_t)setup_packet[5] << 40)
                        | ((uint64_t)setup_packet[6] << 48)
                        | ((uint64_t)setup_packet[7] << 56);
    setup_trb.status = 8;
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
                      | (1 << 6);  // IDT=1, TRT=0 (no data)
    transfer_ring_enqueue(ring, &setup_trb);

    // Status Stage TRB (type 4), DIR=1 (IN) since no data stage
    xhci_trb_t status_trb;
    memory::memset((uint8_t*)&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT)
                       | (1 << 5)    // IOC
                       | (1 << 16);  // DIR=1 (IN)
    transfer_ring_enqueue(ring, &status_trb);

    ring_doorbell(slot_id, XHCI_DOORBELL_TARGET_CONTROL_EP);

    xhci_transfer_event_trb_t* evt = wait_transfer_event(500);
    if (!evt) {
        uart::printf("xhci: control no-data transfer timeout slot=%u\n", (uint32_t)slot_id);
        return false;
    }

    if (evt->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: control no-data transfer failed slot=%u code=%u (%s)\n",
                     (uint32_t)slot_id,
                     (uint32_t)evt->completion_code,
                     completion_code_str(evt->completion_code));
        return false;
    }

    return true;
}

// Send data via Bulk OUT endpoint
static bool bulk_transfer_out(usb_mass_storage_dev* msd, void* data,
                               uintptr_t data_phys, uint32_t length) {
    xhci_transfer_ring* ring = &msd->bulk_out_ring;
    uint8_t out_ep_num = msd->bulk_out_ep & 0x0F;
    uint8_t out_dci = out_ep_num * 2; // OUT direction

    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)data_phys;
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT)
                | (1 << 5); // IOC
    transfer_ring_enqueue(ring, &trb);

    ring_doorbell(msd->slot_id, out_dci);

    xhci_transfer_event_trb_t* evt = wait_transfer_event(2000);
    if (!evt) {
        uart::printf("xhci: bulk OUT timeout\n");
        return false;
    }
    if (evt->completion_code != XHCI_TRB_COMPLETION_SUCCESS &&
        evt->completion_code != 13) {
        uart::printf("xhci: bulk OUT failed code=%u (%s)\n",
                     (uint32_t)evt->completion_code,
                     completion_code_str(evt->completion_code));
        return false;
    }
    return true;
}

// Clear STALL on an endpoint via control transfer (Clear Feature ENDPOINT_HALT)
static bool clear_endpoint_halt(uint8_t slot_id, uint8_t endpoint_address) {
    uint8_t setup[8];
    setup[0] = 0x02;  // bmRequestType: Host-to-Device, Standard, Endpoint
    setup[1] = 0x01;  // bRequest: CLEAR_FEATURE
    setup[2] = 0x00;  // wValue: ENDPOINT_HALT (feature selector 0)
    setup[3] = 0x00;
    setup[4] = endpoint_address;  // wIndex: endpoint address
    setup[5] = 0x00;
    setup[6] = 0x00;  // wLength: 0
    setup[7] = 0x00;

    return control_transfer_no_data(slot_id, setup);
}

// Reset an endpoint after STALL and set new dequeue pointer
static bool reset_endpoint(uint8_t slot_id, uint8_t dci, xhci_transfer_ring* ring) {
    // Reset Endpoint Command
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.control = (XHCI_TRB_TYPE_RESET_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);
    // DCI goes in bits 16-20 of control for this command
    cmd.control |= ((uint32_t)dci << 16);

    xhci_cmd_completion_trb_t* cc = send_command(&cmd, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: reset endpoint failed dci=%u\n", (uint32_t)dci);
        return false;
    }

    // Set TR Dequeue Pointer Command — point to current enqueue position
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    uintptr_t new_dequeue = ring->phys_base + (ring->enqueue_ptr * sizeof(xhci_trb_t));
    cmd.parameter = (uint64_t)(new_dequeue | ring->cycle_bit); // DCS in bit 0
    cmd.control = (XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);
    cmd.control |= ((uint32_t)dci << 16);

    cc = send_command(&cmd, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: set TR dequeue ptr failed dci=%u\n", (uint32_t)dci);
        return false;
    }

    return true;
}

// Recover from STALL on a bulk endpoint
static bool recover_from_stall(usb_mass_storage_dev* msd, uint8_t endpoint_address) {
    uint8_t ep_num = endpoint_address & 0x0F;
    bool is_in = (endpoint_address & 0x80) != 0;
    uint8_t dci = ep_num * 2 + (is_in ? 1 : 0);
    xhci_transfer_ring* ring = is_in ? &msd->bulk_in_ring : &msd->bulk_out_ring;

    uart::printf("xhci: recovering from STALL on ep=0x%x dci=%u\n",
                 (uint32_t)endpoint_address, (uint32_t)dci);

    if (!clear_endpoint_halt(msd->slot_id, endpoint_address)) return false;
    if (!reset_endpoint(msd->slot_id, dci, ring)) return false;

    uart::printf("xhci: STALL recovery OK\n");
    return true;
}

static sint32_t bulk_transfer_in(usb_mass_storage_dev* msd, void* data,
                                  uintptr_t data_phys, uint32_t length) {
    xhci_transfer_ring* ring = &msd->bulk_in_ring;
    uint8_t in_ep_num = msd->bulk_in_ep & 0x0F;
    uint8_t in_dci = in_ep_num * 2 + 1;

    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)data_phys;
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT)
                | (1 << 5); // IOC
    transfer_ring_enqueue(ring, &trb);

    ring_doorbell(msd->slot_id, in_dci);

    xhci_transfer_event_trb_t* evt = wait_transfer_event(2000);
    if (!evt) {
        uart::printf("xhci: bulk IN timeout\n");
        return -1;
    }
    if (evt->completion_code == 6) { // STALL
        uart::printf("xhci: bulk IN STALL, recovering...\n");
        recover_from_stall(msd, msd->bulk_in_ep);
        return -2; // special code for STALL
    }
    if (evt->completion_code != XHCI_TRB_COMPLETION_SUCCESS &&
        evt->completion_code != 13) {
        uart::printf("xhci: bulk IN failed code=%u (%s)\n",
                     (uint32_t)evt->completion_code,
                     completion_code_str(evt->completion_code));
        return -1;
    }

    sint32_t transferred = (sint32_t)length - (sint32_t)evt->transfer_length;
    // Debug dump
    volatile uint8_t* dbg = (volatile uint8_t*)data;
    uart::printf("xhci: bulk_in buf[0..7]: %x %x %x %x %x %x %x %x (transferred=%d)\n",
                 (uint32_t)dbg[0], (uint32_t)dbg[1], (uint32_t)dbg[2], (uint32_t)dbg[3],
                 (uint32_t)dbg[4], (uint32_t)dbg[5], (uint32_t)dbg[6], (uint32_t)dbg[7],
                 transferred);
    return transferred;
    return (sint32_t)length - (sint32_t)evt->transfer_length;
}

// Shared BOT buffers (allocated once)
static usb_cbw* shared_cbw = nullptr;
static usb_csw* shared_csw = nullptr;
static uintptr_t shared_cbw_phys = 0;
static uintptr_t shared_csw_phys = 0;
static uint32_t bot_tag = 1;

static void bot_init_buffers() {
    if (!shared_cbw) {
        shared_cbw = (usb_cbw*)alloc_xhci_memory(sizeof(usb_cbw), 64, 4096);
        shared_cbw_phys = xhci_virt_to_phys(shared_cbw);
        shared_csw = (usb_csw*)alloc_xhci_memory(sizeof(usb_csw), 64, 4096);
        shared_csw_phys = xhci_virt_to_phys(shared_csw);
    }
}

static sint32_t bot_scsi_command(usb_mass_storage_dev* msd,
                                  uint8_t* scsi_cmd, uint8_t scsi_cmd_len,
                                  void* data_buf, uintptr_t data_phys,
                                  uint32_t data_length, uint8_t direction) {
    bot_init_buffers();
    uart::printf("bot: cbw virt=%llx phys=%llx\n",
                 (uint64_t)shared_cbw, (uint64_t)shared_cbw_phys);

    // Build CBW
    memory::memset((uint8_t*)shared_cbw, 0, sizeof(usb_cbw));
    shared_cbw->dCBWSignature = USB_CBW_SIGNATURE;
    shared_cbw->dCBWTag = bot_tag++;
    shared_cbw->dCBWDataTransferLength = data_length;
    shared_cbw->bmCBWFlags = direction;
    shared_cbw->bCBWLUN = 0;
    shared_cbw->bCBWCBLength = scsi_cmd_len;
    memory::memcpy(scsi_cmd, shared_cbw->CBWCB, scsi_cmd_len);

    // Command phase: send CBW
    if (!bulk_transfer_out(msd, shared_cbw, shared_cbw_phys, 31)) {
        uart::printf("bot: CBW send failed\n");
        return -1;
    }

    // Data phase
    if (data_length > 0 && data_buf) {
        if (direction == USB_CBW_FLAG_IN) {
            sint32_t got = bulk_transfer_in(msd, data_buf, data_phys, data_length);
            if (got == -2) {
                // STALL recovered, try to read CSW anyway
                goto read_csw;
            }
            if (got < 0) {
                uart::printf("bot: data IN failed\n");
                return -1;
            }
        } else {
            if (!bulk_transfer_out(msd, data_buf, data_phys, data_length)) {
                uart::printf("bot: data OUT failed\n");
                return -1;
            }
        }
    }

read_csw:
    // Status phase: receive CSW
    memory::memset((uint8_t*)shared_csw, 0, sizeof(usb_csw));
    sint32_t csw_got = bulk_transfer_in(msd, shared_csw, shared_csw_phys, 13);
    uart::printf("bot: CSW raw: %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
                 ((uint8_t*)shared_csw)[0], ((uint8_t*)shared_csw)[1],
                 ((uint8_t*)shared_csw)[2], ((uint8_t*)shared_csw)[3],
                 ((uint8_t*)shared_csw)[4], ((uint8_t*)shared_csw)[5],
                 ((uint8_t*)shared_csw)[6], ((uint8_t*)shared_csw)[7],
                 ((uint8_t*)shared_csw)[8], ((uint8_t*)shared_csw)[9],
                 ((uint8_t*)shared_csw)[10], ((uint8_t*)shared_csw)[11],
                 ((uint8_t*)shared_csw)[12]);
    if (csw_got < 13) {
        uart::printf("bot: CSW receive failed (got=%d)\n", csw_got);
        return -1;
    }

    if (shared_csw->dCSWSignature != USB_CSW_SIGNATURE) {
        uart::printf("bot: invalid CSW signature 0x%x\n", shared_csw->dCSWSignature);
        return -1;
    }

    if (shared_csw->bCSWStatus != 0) {
        uart::printf("bot: CSW status=%u residue=%u\n",
                     (uint32_t)shared_csw->bCSWStatus, shared_csw->dCSWDataResidue);
        return (sint32_t)shared_csw->bCSWStatus;
    }

    return 0;
}

static bool scsi_inquiry(usb_mass_storage_dev* msd) {
    uint8_t* data = (uint8_t*)alloc_xhci_memory(36, 64, 4096);
    uintptr_t data_phys = xhci_virt_to_phys(data);

    // Fill with pattern to detect if DMA actually writes
    for (int i = 0; i < 36; i++) data[i] = 0xAA;

    uint8_t cmd[6];
    memory::memset(cmd, 0, 6);
    cmd[0] = SCSI_INQUIRY;
    cmd[4] = 36;

    sint32_t result = bot_scsi_command(msd, cmd, 6, data, data_phys, 36, USB_CBW_FLAG_IN);
    
    uart::printf("scsi: INQUIRY data (volatile read):\n  ");
    volatile uint8_t* vdata = (volatile uint8_t*)data;
    for (int i = 0; i < 36; i++)
        uart::printf("%x ", (uint32_t)vdata[i]);
    uart::printf("\n");

    if (result != 0) {
        uart::printf("scsi: INQUIRY failed (%d)\n", result);
        return false;
    }

    char vendor[9], product[17];
    memory::memcpy(data + 8, (uint8_t*)vendor, 8);
    vendor[8] = '\0';
    memory::memcpy(data + 16, (uint8_t*)product, 16);
    product[16] = '\0';

    uart::printf("scsi: INQUIRY OK: vendor='%s' product='%s'\n", vendor, product);
    return true;
}

static bool scsi_test_unit_ready(usb_mass_storage_dev* msd) {
    uint8_t cmd[6];
    memory::memset(cmd, 0, 6);
    cmd[0] = SCSI_TEST_UNIT_READY;

    // No data phase
    sint32_t result = bot_scsi_command(msd, cmd, 6, nullptr, 0, 0, USB_CBW_FLAG_OUT);
    if (result != 0) {
        uart::printf("scsi: TEST UNIT READY failed (%d), retrying...\n", result);
        // Retry a few times (device may need time to spin up)
        for (int i = 0; i < 5; i++) {
            delay_ms(500);
            result = bot_scsi_command(msd, cmd, 6, nullptr, 0, 0, USB_CBW_FLAG_OUT);
            if (result == 0) break;
        }
        if (result != 0) return false;
    }

    uart::printf("scsi: TEST UNIT READY OK\n");
    return true;
}

static bool scsi_read_capacity(usb_mass_storage_dev* msd,
                                uint32_t* out_last_lba, uint32_t* out_block_size) {
    uint8_t* data = (uint8_t*)alloc_xhci_memory(8, 64, 4096);
    uintptr_t data_phys = xhci_virt_to_phys(data);

    // Fill with pattern
    for (int i = 0; i < 8; i++) data[i] = 0xBB;

    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_READ_CAPACITY_10;

    sint32_t result = bot_scsi_command(msd, cmd, 10, data, data_phys, 8, USB_CBW_FLAG_IN);

    uart::printf("scsi: READ_CAPACITY data: ");
    for (int i = 0; i < 8; i++)
        uart::printf("%x ", (uint32_t)data[i]);
    uart::printf("\n");

    if (result != 0) {
        uart::printf("scsi: READ CAPACITY failed (%d)\n", result);
        return false;
    }

    *out_last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                  | ((uint32_t)data[2] << 8)  | (uint32_t)data[3];
    *out_block_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16)
                    | ((uint32_t)data[6] << 8)  | (uint32_t)data[7];

    uint64_t total_bytes = ((uint64_t)*out_last_lba + 1) * (uint64_t)*out_block_size;
    uint32_t size_mb = (uint32_t)(total_bytes / (1024 * 1024));

    uart::printf("scsi: READ CAPACITY OK: last_lba=%u block_size=%u total=%u MB\n",
                 *out_last_lba, *out_block_size, size_mb);
    return true;
}

// Read sectors from the device
static bool scsi_read_10(usb_mass_storage_dev* msd, uint32_t lba,
                          uint16_t sector_count, void* buffer, uintptr_t buffer_phys) {
    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_READ_10;
    // LBA (big-endian)
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)(lba);
    // Transfer length in sectors (big-endian)
    cmd[7] = (uint8_t)(sector_count >> 8);
    cmd[8] = (uint8_t)(sector_count);

    // Use actual block size from the device (passed separately or use 512 as fallback)
    uint32_t byte_count = (uint32_t)sector_count * 512;

    sint32_t result = bot_scsi_command(msd, cmd, 10, buffer, buffer_phys,
                                        byte_count, USB_CBW_FLAG_IN);
    if (result != 0) {
        uart::printf("scsi: READ(10) failed lba=%u count=%u (%d)\n",
                     lba, (uint32_t)sector_count, result);
        return false;
    }
    return true;
}

// Write sectors to the device
static bool scsi_write_10(usb_mass_storage_dev* msd, uint32_t lba,
                           uint16_t sector_count, void* buffer, uintptr_t buffer_phys) {
    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_WRITE_10;
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)(lba);
    cmd[7] = (uint8_t)(sector_count >> 8);
    cmd[8] = (uint8_t)(sector_count);

    // Use actual block size from the device (passed separately or use 512 as fallback)
    uint32_t byte_count = (uint32_t)sector_count * 512;

    sint32_t result = bot_scsi_command(msd, cmd, 10, buffer, buffer_phys,
                                        byte_count, USB_CBW_FLAG_OUT);
    if (result != 0) {
        uart::printf("scsi: WRITE(10) failed lba=%u count=%u (%d)\n",
                     lba, (uint32_t)sector_count, result);
        return false;
    }
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

static bool evaluate_context(uint8_t slot_id, uint16_t new_max_packet_size) {
    xhci_input_context* input_ctx = alloc_input_context();
    if (!input_ctx) return false;

    // Only evaluating EP0
    input_ctx->control_context.add_flags = (1 << 1); // EP0 only
    input_ctx->device_context.control_ep_context.max_packet_size = new_max_packet_size;

    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(&cmd, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: evaluate context failed slot=%u\n", (uint32_t)slot_id);
        return false;
    }

    uart::printf("xhci: evaluate context OK, max_packet_size=%u\n",
                 (uint32_t)new_max_packet_size);
    return true;
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

static bool get_device_descriptor(uint8_t slot_id, uint8_t port_speed) {
    // Allocate buffer for descriptor (aligned, physically contiguous)
    usb_device_descriptor* desc = (usb_device_descriptor*)alloc_xhci_memory(
        sizeof(usb_device_descriptor), 64, 4096);
    uintptr_t desc_phys = xhci_virt_to_phys(desc);

    // USB Setup Packet: GET_DESCRIPTOR (Device), 8 bytes first
    uint8_t setup[8];
    setup[0] = 0x80;       // bmRequestType: Device-to-Host, Standard, Device
    setup[1] = 0x06;       // bRequest: GET_DESCRIPTOR
    setup[2] = 0x00;       // wValue low: descriptor index 0
    setup[3] = 0x01;       // wValue high: DEVICE descriptor type
    setup[4] = 0x00;       // wIndex low
    setup[5] = 0x00;       // wIndex high
    setup[6] = 8;          // wLength low: first 8 bytes only
    setup[7] = 0;          // wLength high

    sint32_t got = control_transfer_in(slot_id, setup, desc, desc_phys, 8);
    if (got < 8) {
        uart::printf("xhci: get device desc (8B) failed, got=%d\n", got);
        return false;
    }

    // Check if max_packet_size needs updating
    uint8_t real_max_pkt = desc->bMaxPacketSize0;
    uint16_t current_max_pkt = max_packet_size_for_speed(port_speed);

    uart::printf("xhci: bMaxPacketSize0=%u (current=%u)\n",
                 (uint32_t)real_max_pkt, (uint32_t)current_max_pkt);

    // For SuperSpeed, bMaxPacketSize0 is encoded as 2^N (9 means 512)
    uint16_t actual_max_pkt = real_max_pkt;
    if (port_speed >= 4 && real_max_pkt <= 16) {
        actual_max_pkt = (uint16_t)(1 << real_max_pkt);
    }

    if (actual_max_pkt != current_max_pkt) {
        uart::printf("xhci: updating max_packet_size %u -> %u\n",
                     (uint32_t)current_max_pkt, (uint32_t)actual_max_pkt);
        if (!evaluate_context(slot_id, actual_max_pkt)) return false;
    }

    // Now read full 18-byte descriptor
    memory::memset((uint8_t*)desc, 0, sizeof(usb_device_descriptor));
    setup[6] = 18; // wLength = 18
    got = control_transfer_in(slot_id, setup, desc, desc_phys, 18);
    if (got < 18) {
        uart::printf("xhci: get device desc (18B) failed, got=%d\n", got);
        return false;
    }

    uart::printf("xhci: device descriptor slot=%u:\n", (uint32_t)slot_id);
    uart::printf("  bcdUSB         : %x\n", (uint32_t)desc->bcdUSB);
    uart::printf("  class/sub/proto: %u/%u/%u\n",
                 (uint32_t)desc->bDeviceClass,
                 (uint32_t)desc->bDeviceSubClass,
                 (uint32_t)desc->bDeviceProtocol);
    uart::printf("  VID:PID        : %x:%x\n",
                 (uint32_t)desc->idVendor,
                 (uint32_t)desc->idProduct);
    uart::printf("  bMaxPacketSize0: %u\n", (uint32_t)desc->bMaxPacketSize0);
    uart::printf("  bNumConfigs    : %u\n", (uint32_t)desc->bNumConfigurations);

    return true;
}

static bool get_config_descriptor(uint8_t slot_id, uint8_t port_speed, uint8_t port_index) {
    // Read first 9 bytes to get wTotalLength
    uint8_t* buf = (uint8_t*)alloc_xhci_memory(512, 64, 4096);
    uintptr_t buf_phys = xhci_virt_to_phys(buf);

    uint8_t setup[8];
    setup[0] = 0x80;  // bmRequestType: Device-to-Host, Standard, Device
    setup[1] = 0x06;  // bRequest: GET_DESCRIPTOR
    setup[2] = 0x00;  // wValue low: index 0
    setup[3] = 0x02;  // wValue high: CONFIGURATION descriptor
    setup[4] = 0x00;  // wIndex
    setup[5] = 0x00;
    setup[6] = 9;     // wLength: just the header
    setup[7] = 0;

    sint32_t got = control_transfer_in(slot_id, setup, buf, buf_phys, 9);
    if (got < 9) {
        uart::printf("xhci: get config desc header failed, got=%d\n", got);
        return false;
    }

    usb_config_descriptor* cfg = (usb_config_descriptor*)buf;
    uint16_t total_len = cfg->wTotalLength;
    uint8_t config_value = cfg->bConfigurationValue;

    uart::printf("xhci: config descriptor slot=%u: wTotalLength=%u bNumInterfaces=%u bConfigValue=%u\n",
                 (uint32_t)slot_id, (uint32_t)total_len,
                 (uint32_t)cfg->bNumInterfaces, (uint32_t)config_value);

    if (total_len > 512) total_len = 512;

    // Read full configuration descriptor
    memory::memset(buf, 0, 512);
    setup[6] = (uint8_t)(total_len & 0xFF);
    setup[7] = (uint8_t)(total_len >> 8);

    got = control_transfer_in(slot_id, setup, buf, buf_phys, total_len);
    if (got < (sint32_t)total_len) {
        uart::printf("xhci: get config desc full failed, got=%d expected=%u\n",
                     got, (uint32_t)total_len);
        return false;
    }

    // Parse descriptor chain
    uint16_t offset = 0;
    bool in_mass_storage = false;
    uint8_t bulk_in_ep = 0;
    uint8_t bulk_out_ep = 0;
    uint16_t bulk_in_max_pkt = 0;
    uint16_t bulk_out_max_pkt = 0;
    uint8_t iface_number = 0;

    while (offset + 2 <= total_len) {
        uint8_t desc_len = buf[offset];
        uint8_t desc_type = buf[offset + 1];

        if (desc_len == 0) break; // safety

        if (desc_type == USB_DESC_TYPE_INTERFACE && desc_len >= 9) {
            usb_interface_descriptor* iface = (usb_interface_descriptor*)(buf + offset);

            uart::printf("xhci:   interface %u: class=%u sub=%u proto=%u endpoints=%u\n",
                         (uint32_t)iface->bInterfaceNumber,
                         (uint32_t)iface->bInterfaceClass,
                         (uint32_t)iface->bInterfaceSubClass,
                         (uint32_t)iface->bInterfaceProtocol,
                         (uint32_t)iface->bNumEndpoints);

            if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                iface->bInterfaceSubClass == USB_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_PROTOCOL_BBB) {
                uart::printf("xhci:   -> Mass Storage BBB found!\n");
                in_mass_storage = true;
                iface_number = iface->bInterfaceNumber;
            } else {
                in_mass_storage = false;
            }
        }

        if (desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7 && in_mass_storage) {
            usb_endpoint_descriptor* ep = (usb_endpoint_descriptor*)(buf + offset);
            uint8_t ep_addr = ep->bEndpointAddress;
            uint8_t ep_type = ep->bmAttributes & 0x03;

            uart::printf("xhci:   endpoint addr=0x%x type=%u maxpkt=%u\n",
                         (uint32_t)ep_addr, (uint32_t)ep_type,
                         (uint32_t)ep->wMaxPacketSize);

            // Bulk endpoint (type 2)
            if (ep_type == 2) {
                if (ep_addr & 0x80) {
                    // IN endpoint
                    bulk_in_ep = ep_addr;
                    bulk_in_max_pkt = ep->wMaxPacketSize;
                } else {
                    // OUT endpoint
                    bulk_out_ep = ep_addr;
                    bulk_out_max_pkt = ep->wMaxPacketSize;
                }
            }
        }

        offset += desc_len;
    }

    // Store mass storage device info if found
    if (bulk_in_ep && bulk_out_ep && mass_storage_count < MAX_MASS_STORAGE_DEVS) {
        usb_mass_storage_dev* msd = &mass_storage_devs[mass_storage_count++];
        msd->slot_id = slot_id;
        msd->port_index = port_index;
        msd->port_speed = port_speed;
        msd->config_value = config_value;
        msd->interface_number = iface_number;
        msd->bulk_in_ep = bulk_in_ep;
        msd->bulk_out_ep = bulk_out_ep;
        msd->bulk_in_max_packet = bulk_in_max_pkt;
        msd->bulk_out_max_packet = bulk_out_max_pkt;
        msd->found = true;

        uart::printf("xhci: mass storage registered: slot=%u bulk_in=0x%x bulk_out=0x%x\n",
                     (uint32_t)slot_id, (uint32_t)bulk_in_ep, (uint32_t)bulk_out_ep);
    }

    return true;
}

static bool configure_mass_storage(usb_mass_storage_dev* msd) {
    uint8_t slot_id = msd->slot_id;

    // Step 1: SET_CONFIGURATION
    uint8_t setup[8];
    setup[0] = 0x00;  // bmRequestType: Host-to-Device, Standard, Device
    setup[1] = 0x09;  // bRequest: SET_CONFIGURATION
    setup[2] = msd->config_value;  // wValue low
    setup[3] = 0x00;               // wValue high
    setup[4] = 0x00;  // wIndex
    setup[5] = 0x00;
    setup[6] = 0x00;  // wLength
    setup[7] = 0x00;

    uart::printf("xhci: SET_CONFIGURATION %u for slot %u\n",
                 (uint32_t)msd->config_value, (uint32_t)slot_id);

    if (!control_transfer_no_data(slot_id, setup)) {
        uart::printf("xhci: SET_CONFIGURATION failed\n");
        return false;
    }
    uart::printf("xhci: SET_CONFIGURATION OK\n");

    // Step 2: Allocate transfer rings for bulk endpoints
    transfer_ring_init(&msd->bulk_in_ring, XHCI_TRANSFER_RING_TRB_COUNT);
    transfer_ring_init(&msd->bulk_out_ring, XHCI_TRANSFER_RING_TRB_COUNT);

    // Step 3: Build Input Context for Configure Endpoint Command
    // Calculate DCI (Device Context Index) for each endpoint
    // DCI = endpoint_number * 2 + direction (0=OUT, 1=IN)
    // For bulk_in_ep=0x81: ep_num=1, dir=IN(1), DCI = 1*2+1 = 3
    // For bulk_out_ep=0x02: ep_num=2, dir=OUT(0), DCI = 2*2+0 = 4
    uint8_t in_ep_num = msd->bulk_in_ep & 0x0F;
    uint8_t out_ep_num = msd->bulk_out_ep & 0x0F;
    uint8_t in_dci = in_ep_num * 2 + 1;   // IN direction
    uint8_t out_dci = out_ep_num * 2;      // OUT direction

    // Max DCI determines context_entries in slot context
    uint8_t max_dci = in_dci > out_dci ? in_dci : out_dci;

    uart::printf("xhci: bulk_in DCI=%u, bulk_out DCI=%u, max_dci=%u\n",
                 (uint32_t)in_dci, (uint32_t)out_dci, (uint32_t)max_dci);

    xhci_input_context* input_ctx = alloc_input_context();
    if (!input_ctx) return false;

    // Add flags: Slot (bit 0) + both endpoint DCIs
    input_ctx->control_context.add_flags = (1 << 0) | (1 << in_dci) | (1 << out_dci);
    input_ctx->control_context.drop_flags = 0;

    // Update Slot Context
    xhci_slot_context* slot = &input_ctx->device_context.slot_context;
    // Copy current slot context from output context
    xhci_device_context* out_ctx = (xhci_device_context*)dcbaa_virt[slot_id];
    *slot = out_ctx->slot_context;
    slot->context_entries = max_dci;

    // Bulk IN endpoint context (ep[] is 0-indexed, DCI 2 = ep[0], DCI 3 = ep[1], etc.)
    // control_ep_context is DCI 1, ep[0] is DCI 2, ep[1] is DCI 3 ...
    xhci_endpoint_context* ep_in = &input_ctx->device_context.ep[in_dci - 2];
    ep_in->endpoint_type = XHCI_EP_TYPE_BULK_IN;
    ep_in->max_packet_size = msd->bulk_in_max_packet;
    ep_in->max_burst_size = 0;
    ep_in->error_count = 3;
    ep_in->average_trb_length = 1024;
    ep_in->transfer_ring_dequeue_ptr = msd->bulk_in_ring.phys_base | 1; // DCS=1

    // Bulk OUT endpoint context
    xhci_endpoint_context* ep_out = &input_ctx->device_context.ep[out_dci - 2];
    ep_out->endpoint_type = XHCI_EP_TYPE_BULK_OUT;
    ep_out->max_packet_size = msd->bulk_out_max_packet;
    ep_out->max_burst_size = 0;
    ep_out->error_count = 3;
    ep_out->average_trb_length = 1024;
    ep_out->transfer_ring_dequeue_ptr = msd->bulk_out_ring.phys_base | 1; // DCS=1

    // Step 4: Send Configure Endpoint Command
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(&cmd, 500);
    if (!cc) {
        uart::printf("xhci: configure endpoint timeout\n");
        return false;
    }

    if (cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS) {
        uart::printf("xhci: configure endpoint FAILED code=%u (%s)\n",
                     (uint32_t)cc->completion_code,
                     completion_code_str(cc->completion_code));
        return false;
    }

    msd->configured = true;
    uart::printf("xhci: configure endpoint OK, device ready for BOT/SCSI\n");
    return true;
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

    // Step 6: Get Device Descriptor
    get_device_descriptor(slot_id, port_speed);

    // Step 7: Get Configuration Descriptor and find Mass Storage
    get_config_descriptor(slot_id, port_speed, port_index);
}

namespace xhci {
    bool init() {
        shared_cbw = nullptr;
        shared_csw = nullptr;
        shared_cbw_phys = 0;
        shared_csw_phys = 0;
        bot_tag = 1;

        PCIDevice* dev = pci::find(PCI_CLASS_SERIAL, 0x03, 0x30);
        if (!dev) { uart::printf("xhci: no xHCI controller found\n"); return false; }

        mass_storage_count = 0;
        memory::memset((uint8_t*)mass_storage_devs, 0, sizeof(mass_storage_devs));

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

        // Configure all found Mass Storage devices
        for (uint8_t i = 0; i < mass_storage_count; i++) {
            uart::printf("xhci: configuring mass storage device %u (slot=%u)\n",
                         (uint32_t)i, (uint32_t)mass_storage_devs[i].slot_id);
            configure_mass_storage(&mass_storage_devs[i]);
        }


        // Test first mass storage device
        if (mass_storage_count > 0) {
            usb_mass_storage_dev* msd = &mass_storage_devs[0];
            if (msd->configured) {
                uart::printf("\n\nxhci: === Testing Mass Storage (slot=%u) ===\n",
                             (uint32_t)msd->slot_id);

                if (!scsi_inquiry(msd)) return false;
                if (!scsi_test_unit_ready(msd)) return false;

                uint32_t last_lba, block_size;
                if (!scsi_read_capacity(msd, &last_lba, &block_size)) return false;

                // Read sector 0 (MBR / partition table)
                uint8_t* sector = (uint8_t*)alloc_xhci_memory(512, 64, 4096);
                uintptr_t sector_phys = xhci_virt_to_phys(sector);

                uart::printf("scsi: sector buf virt=%llx phys=%llx\n",
                             (uint64_t)sector, (uint64_t)sector_phys);
                // Also check: are virt and phys the same?
                if ((uint64_t)sector != sector_phys) {
                    uart::printf("scsi: WARNING: virt != phys! DMA will write to phys but we read from virt\n");
                }
                
                if (scsi_read_10(msd, 0, 1, sector, sector_phys)) {
                    uart::printf("scsi: READ sector 0 OK, first 16 bytes:\n  ");
                    for (int i = 0; i < 16; i++)
                        uart::printf("%x ", (uint32_t)sector[i]);
                    uart::printf("\n");

                    // Check MBR signature
                    if (sector[510] == 0x55 && sector[511] == 0xAA)
                        uart::printf("scsi: MBR signature found (0x55AA)\n");
                    else
                        uart::printf("scsi: no MBR signature (got 0x%x 0x%x)\n",
                                     (uint32_t)sector[510], (uint32_t)sector[511]);
                }

                // Test write: read sector 1, modify, write back, verify
                uint8_t* test_buf = (uint8_t*)alloc_xhci_memory(512, 64, 4096);
                uintptr_t test_phys = xhci_virt_to_phys(test_buf);

                uart::printf("scsi: write test on sector 1...\n");
                if (scsi_read_10(msd, 1, 1, test_buf, test_phys)) {
                    uart::printf("scsi: sector 1 before write: %x %x %x %x\n",
                                 (uint32_t)test_buf[0], (uint32_t)test_buf[1],
                                 (uint32_t)test_buf[2], (uint32_t)test_buf[3]);

                    // Write a test pattern
                    test_buf[0] = 0xDE;
                    test_buf[1] = 0xAE;
                    test_buf[2] = 0xBE;
                    test_buf[3] = 0xEF;

                    if (scsi_write_10(msd, 1, 1, test_buf, test_phys)) {
                        uart::printf("scsi: WRITE sector 1 OK\n");

                        // Read back to verify
                        memory::memset(test_buf, 0, 512);
                        if (scsi_read_10(msd, 1, 1, test_buf, test_phys)) {
                            uart::printf("scsi: verify: %x %x %x %x\n",
                                         (uint32_t)test_buf[0], (uint32_t)test_buf[1],
                                         (uint32_t)test_buf[2], (uint32_t)test_buf[3]);
                            if (test_buf[0] == 0xDE && test_buf[1] == 0xAE &&
                                test_buf[2] == 0xBE && test_buf[3] == 0xEF) {
                                uart::printf("scsi: === READ/WRITE VERIFIED OK ===\n");
                            }
                        }
                    }
                }
            }
        }


        return true;
    }
}