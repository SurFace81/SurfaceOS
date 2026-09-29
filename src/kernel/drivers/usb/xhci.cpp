// xHCI host controller: registers, rings, commands, events, ports and
// device contexts. What goes over the wire - descriptors, classes - is the
// USB core's (usb.cpp); this file only moves TRBs.

#include "../../../include/drivers/usb/xhci.h"
#include "../../../include/drivers/pit.h"

// Utility functions

// Every timeout in this driver is expressed in milliseconds and ends up
// here. It used to be a fixed count of `pause` instructions, which is not a
// unit of time: the same "500 ms" timeout was a fraction of a second on a
// 3 GHz laptop and many seconds under QEMU's interpreter. Enumeration that
// worked in one place would time out in the other.
//
// Now it waits on the PIT. That needs interrupts enabled, which is why
// the console no longer runs commands under cli.
static void delay_ms(uint32_t ms)
{
    if (ms == 0)
        return;

    uint32_t hz = pit::real_frequency();
    if (hz)
    {
        uint64_t target = pit::ticks() + ((uint64_t)ms * hz + 999) / 1000;

        // Bounded so a stopped timer degrades into a spin rather than a hang.
        uint64_t guard = (uint64_t)ms * 20000000ULL + 1000000ULL;
        while (pit::ticks() < target && guard--)
            asm volatile("pause");
        return;
    }

    // Timer not up yet (very early init): fall back to a crude spin.
    for (uint32_t i = 0; i < ms; i++)
        for (volatile uint32_t j = 0; j < 100000; j++)
            asm volatile("pause");
}

static void write_mmio64(volatile uint64_t* reg, uint64_t val)
{
    volatile uint32_t* reg32 = (volatile uint32_t*)reg;
    reg32[0] = (uint32_t)(val & 0xFFFFFFFF);
    reg32[1] = (uint32_t)(val >> 32);
}

static uint64_t read_mmio64(volatile uint64_t* reg)
{
    volatile uint32_t* reg32 = (volatile uint32_t*)reg;
    uint32_t lo = reg32[0];
    uint32_t hi = reg32[1];
    return ((uint64_t)hi << 32) | lo;
}

// Memory allocation with xHCI alignment and boundary requirements
static void* alloc_xhci_memory(size_t size, size_t alignment, size_t boundary)
{
    // A block larger than `boundary` cannot help crossing one; moving it
    // up to the next boundary would run it past the end of what kmalloc
    // gave (and over the neighbouring heap blocks).
    if (size == 0 || alignment == 0 || (boundary && size > boundary))
    {
        uart::printf("xhci: bad alloc params size=%u align=%u boundary=%u\n", (uint32_t)size,
                     (uint32_t)alignment, (uint32_t)boundary);
        while (1);
    }

    size_t total = size + alignment + boundary + sizeof(void*);
    void* raw = kmalloc(total);
    if (!raw)
    {
        uart::printf("xhci: alloc failed size=%u\n", (uint32_t)size);
        while (1);
    }

    uintptr_t base = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (base + alignment - 1) & ~(alignment - 1);

    if (boundary > 0)
    {
        uintptr_t start_region = aligned / boundary;
        uintptr_t end_region = (aligned + size - 1) / boundary;
        if (start_region != end_region)
            aligned = (end_region * boundary + alignment - 1) & ~(alignment - 1);
    }

    ((void**)aligned)[-1] = raw;
    memory::memset((uint8_t*)aligned, 0x00, size);
    return (void*)aligned;
}

static void free_xhci_memory(void* ptr)
{
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    kfree(raw);
}

static uintptr_t xhci_virt_to_phys(void* vaddr)
{
    // Kept as a separate step from the walk below so a bad translation is
    // reported once, here, instead of turning into a silent DMA to nowhere.
    // DMA buffers come from kmalloc, i.e. the kernel heap in the direct map,
    // where virt_to_phys() would do - but walk the tables rather than assume
    // it, so a buffer anywhere else (a kernel-image static, a future vmalloc
    // area) cannot hand the controller a bogus bus address silently.
    uintptr_t phys = (uintptr_t)paging::virtual_to_phys((uint64_t)vaddr);
    if (phys == 0)
        uart::printf("xhci: virt %llx has no physical mapping\n", (uint64_t)vaddr);
    return phys;
}

static uintptr_t xhci_map_mmio(uint64_t bar_addr, uint64_t bar_size)
{
    return (uintptr_t)paging::map_mmio_region(bar_addr, bar_size);
}

static uint64_t get_bar_size_64(PCIDevice* dev, int bar_index)
{
    uint8_t off_lo = PCI_BAR0 + bar_index * 4;
    uint8_t off_hi = PCI_BAR0 + (bar_index + 1) * 4;
    uint32_t orig_lo = pci::read32(dev, off_lo);
    uint32_t orig_hi = pci::read32(dev, off_hi);
    bool is_64bit = ((orig_lo >> 1) & 0x3) == 0x02;

    pci::write32(dev, off_lo, 0xFFFFFFFF);
    uint32_t size_lo = pci::read32(dev, off_lo);
    pci::write32(dev, off_lo, orig_lo);

    uint64_t size_mask;
    if (is_64bit)
    {
        pci::write32(dev, off_hi, 0xFFFFFFFF);
        uint32_t size_hi = pci::read32(dev, off_hi);
        pci::write32(dev, off_hi, orig_hi);
        size_mask = ((uint64_t)size_hi << 32) | (size_lo & 0xFFFFFFF0);
    }
    else
    {
        size_mask = (uint64_t)(size_lo & 0xFFFFFFF0);
        size_mask |= 0xFFFFFFFF00000000ULL;
    }
    return (~size_mask) + 1;
}

static const char* completion_code_str(uint8_t code)
{
    switch (code)
    {
        case 0:
            return "INVALID";
        case 1:
            return "SUCCESS";
        case 2:
            return "DATA_BUFFER_ERROR";
        case 3:
            return "BABBLE_DETECTED";
        case 4:
            return "USB_TRANSACTION_ERROR";
        case 5:
            return "TRB_ERROR";
        case 6:
            return "STALL_ERROR";
        case 7:
            return "RESOURCE_ERROR";
        case 8:
            return "BANDWIDTH_ERROR";
        case 9:
            return "NO_SLOTS_AVAILABLE";
        case 13:
            return "SHORT_PACKET";
        case 17:
            return "PARAMETER_ERROR";
        case 19:
            return "CONTEXT_STATE_ERROR";
        case 21:
            return "EVENT_RING_FULL";
        case 24:
            return "COMMAND_RING_STOPPED";
        case 25:
            return "COMMAND_ABORTED";
        default:
            return "UNKNOWN";
    }
}
// Internal ring structures

struct xhci_cmd_ring
{
    xhci_trb_t* trbs;
    uintptr_t phys_base;
    size_t max_trb_count;
    size_t enqueue_ptr;
    uint8_t cycle_bit;
};

struct xhci_evt_ring
{
    xhci_trb_t* trbs;
    uintptr_t phys_base;
    xhci_erst_entry* segment_table;
    volatile xhci_interrupter_regs* interrupter;
    size_t segment_trb_count;
    uint64_t dequeue_ptr;
    uint8_t cycle_bit;
};

// Extended capability: Supported Protocol (spec section 7.2)
struct xhci_supported_protocol
{
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

// One xHCI controller: its registers, what its capability registers say,
// and the structures it reads and writes by DMA.
struct xhci_controller
{
    PCIDevice* pci;
    uintptr_t base;
    volatile xhci_cap_regs* cap_regs;
    volatile xhci_op_regs* op_regs;
    volatile xhci_runtime_regs* runtime_regs;
    volatile xhci_doorbell_reg* doorbells;

    uint8_t max_device_slots;       // what we enable, at most XHCI_MAX_SLOTS
    uint8_t max_ports;
    uint16_t max_scratchpad_bufs;   // HCSPARAMS2 spreads this over 10 bits

    // Size of one entry in a slot/endpoint context array. The structures in
    // xhci.h describe the 32-byte layout; a controller that sets
    // HCCPARAMS1.CSZ spaces the very same fields 64 bytes apart, so every
    // context entry has to be addressed by this stride rather than by C
    // array indexing. QEMU reports CSZ = 0, which is why hardcoding 32
    // survived until real hardware.
    uint32_t ctx_entry_size;
    uint32_t xecp_offset;

    uint64_t* dcbaa;
    uint64_t* dcbaa_virt;

    xhci_cmd_ring cmd_ring;
    xhci_evt_ring evt_ring;

    // Where a Transfer Event goes: the ring of (slot, DCI), at
    // rings[slot * XHCI_MAX_DCI + dci]; null for endpoints not in use.
    xhci_transfer_ring** rings;

    // The command in flight (its TRB's bus address) and, once its
    // completion event arrived, a copy of that event.
    uint64_t cmd_pending;
    volatile bool cmd_done;
    xhci_cmd_completion_trb_t cmd_result;

    uint8_t usb3_ports[XHCI_MAX_USB3_PORTS];
    uint8_t usb3_port_count;
};

static xhci_controller controllers[MAX_XHCI_CONTROLLERS];
static uint8_t controller_count = 0;

// Doorbell helpers

static void ring_doorbell(xhci_controller* hc, uint8_t slot, uint8_t target)
{
    hc->doorbells[slot].raw = (uint32_t)target;
}

static void ring_command_doorbell(xhci_controller* hc)
{
    ring_doorbell(hc, 0, XHCI_DOORBELL_TARGET_COMMAND_RING);
}

// Command ring operations

static void cmd_ring_init(xhci_cmd_ring* ring, size_t max_trbs)
{
    ring->max_trb_count = max_trbs;
    ring->cycle_bit = XHCI_CRCR_RING_CYCLE_STATE;
    ring->enqueue_ptr = 0;

    ring->trbs =
        (xhci_trb_t*)alloc_xhci_memory(max_trbs * sizeof(xhci_trb_t), XHCI_CMD_RING_ALIGNMENT, XHCI_CMD_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->trbs[max_trbs - 1].parameter = ring->phys_base;
    ring->trbs[max_trbs - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;
}

static void cmd_ring_enqueue(xhci_cmd_ring* ring, xhci_trb_t* trb)
{
    trb->cycle_bit = ring->cycle_bit;
    ring->trbs[ring->enqueue_ptr] = *trb;

    if (++ring->enqueue_ptr == ring->max_trb_count - 1)
    {
        ring->trbs[ring->max_trb_count - 1].control =
            (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;
        ring->enqueue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
}

// Event ring operations

static void evt_ring_update_erdp(xhci_evt_ring* ring)
{
    uint64_t addr = ring->phys_base + (ring->dequeue_ptr * sizeof(xhci_trb_t));
    write_mmio64(&ring->interrupter->erdp, addr);
}

static void evt_ring_init(xhci_evt_ring* ring, size_t max_trbs, volatile xhci_interrupter_regs* interrupter)
{
    ring->interrupter = interrupter;
    ring->segment_trb_count = max_trbs;
    ring->cycle_bit = XHCI_CRCR_RING_CYCLE_STATE;
    ring->dequeue_ptr = 0;

    ring->trbs =
        (xhci_trb_t*)alloc_xhci_memory(max_trbs * sizeof(xhci_trb_t), XHCI_EVT_RING_ALIGNMENT, XHCI_EVT_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->segment_table =
        (xhci_erst_entry*)alloc_xhci_memory(sizeof(xhci_erst_entry), XHCI_ERST_ALIGNMENT, XHCI_ERST_BOUNDARY);

    ring->segment_table[0].ring_segment_base_address = ring->phys_base;
    ring->segment_table[0].ring_segment_size = max_trbs;
    ring->segment_table[0].rsvd = 0;

    interrupter->erstsz = 1;
    evt_ring_update_erdp(ring);
    write_mmio64(&interrupter->erstba, xhci_virt_to_phys(ring->segment_table));
}

static xhci_trb_t* evt_ring_dequeue_trb(xhci_evt_ring* ring)
{
    if (ring->trbs[ring->dequeue_ptr].cycle_bit != ring->cycle_bit)
        return nullptr;

    xhci_trb_t* trb = &ring->trbs[ring->dequeue_ptr];
    if (++ring->dequeue_ptr == ring->segment_trb_count)
    {
        ring->dequeue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
    return trb;
}

static void evt_ring_advance_erdp(xhci_controller* hc)
{
    evt_ring_update_erdp(&hc->evt_ring);
    uint64_t erdp = read_mmio64(&hc->evt_ring.interrupter->erdp);
    erdp |= XHCI_ERDP_EHB;
    write_mmio64(&hc->evt_ring.interrupter->erdp, erdp);
}

// Transfer ring operations

static void transfer_ring_init(xhci_transfer_ring* ring, size_t max_trbs)
{
    ring->max_trb_count = max_trbs;
    ring->cycle_bit = 1;
    ring->enqueue_ptr = 0;

    ring->trbs = (xhci_trb_t*)alloc_xhci_memory(max_trbs * sizeof(xhci_trb_t), XHCI_TRANSFER_RING_ALIGNMENT,
                                                XHCI_TRANSFER_RING_BOUNDARY);
    ring->phys_base = xhci_virt_to_phys(ring->trbs);

    ring->trbs[max_trbs - 1].parameter = ring->phys_base;
    ring->trbs[max_trbs - 1].status = 0;
    ring->trbs[max_trbs - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;
}

static void transfer_ring_enqueue(xhci_transfer_ring* ring, xhci_trb_t* trb)
{
    trb->cycle_bit = ring->cycle_bit;
    ring->trbs[ring->enqueue_ptr] = *trb;

    if (++ring->enqueue_ptr == ring->max_trb_count - 1)
    {
        ring->trbs[ring->max_trb_count - 1].control =
            (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | ring->cycle_bit;
        ring->enqueue_ptr = 0;
        ring->cycle_bit = !ring->cycle_bit;
    }
}

// IRQ acknowledgment

static void acknowledge_irq(xhci_controller* hc, uint8_t interrupter)
{
    volatile xhci_interrupter_regs* ir = &hc->runtime_regs->ir[interrupter];
    uint32_t iman = ir->iman;
    iman |= XHCI_IMAN_INTERRUPT_PENDING;
    ir->iman = iman;
    hc->op_regs->usbsts = XHCI_USBSTS_EINT;
}

// Event processing
//
// process_events() is the only reader of the event ring. Every event goes
// where it belongs - a transfer's to the ring of its slot and endpoint, a
// command's completion to the command in flight - and whoever waits for one
// looks there. An event for someone else can therefore arrive in the middle
// of any wait without being lost or taken for the one waited on.

static xhci_transfer_ring* ring_of(xhci_controller* hc, uint8_t slot, uint8_t dci)
{
    if (!hc->rings || slot == 0 || slot > hc->max_device_slots || dci == 0 || dci >= XHCI_MAX_DCI)
        return nullptr;
    return hc->rings[slot * XHCI_MAX_DCI + dci];
}

static void set_ring(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring)
{
    hc->rings[slot * XHCI_MAX_DCI + dci] = ring;
}

static void on_transfer_event(xhci_controller* hc, const xhci_transfer_event_trb_t* te)
{
    xhci_transfer_ring* ring = ring_of(hc, te->slot_id, te->endpoint_id);
    if (!ring)
    {
        uart::printf("xhci: transfer event for slot %u dci %u, which has no ring\n",
                     (uint32_t)te->slot_id, (uint32_t)te->endpoint_id);
        return;
    }

    uint8_t cc = te->completion_code;
    if (cc == XHCI_TRB_COMPLETION_SHORT_PACKET)
        ring->residue = te->transfer_length;

    // A transfer is over at its IOC TRB - or at an error, where the endpoint
    // halts. A short packet in a control transfer's data stage is reported
    // on its own and the status stage still follows.
    if (cc == XHCI_TRB_COMPLETION_SUCCESS || cc == XHCI_TRB_COMPLETION_SHORT_PACKET)
    {
        uint64_t index = (te->trb_pointer - ring->phys_base) / sizeof(xhci_trb_t);
        if (index >= ring->max_trb_count || !ring->trbs[index].interrupt_on_completion)
            return;
    }

    ring->cc = cc;
    ring->done = true;
}

static void on_command_completion(xhci_controller* hc, const xhci_cmd_completion_trb_t* cc)
{
    if (hc->cmd_done || cc->command_trb_pointer != hc->cmd_pending)
    {
        uart::printf("xhci: completion for command %llx, which nobody waits for\n",
                     (uint64_t)cc->command_trb_pointer);
        return;
    }
    hc->cmd_result = *cc;
    hc->cmd_done = true;
}

static void process_events(xhci_controller* hc)
{
    bool any = false;
    xhci_trb_t* trb;
    while ((trb = evt_ring_dequeue_trb(&hc->evt_ring)) != nullptr)
    {
        any = true;
        switch (trb->trb_type)
        {
            case XHCI_TRB_TYPE_TRANSFER_EVENT:
                on_transfer_event(hc, (xhci_transfer_event_trb_t*)trb);
                break;
            case XHCI_TRB_TYPE_CMD_COMPLETION_EVENT:
                on_command_completion(hc, (xhci_cmd_completion_trb_t*)trb);
                break;
            case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT:
                // Ports are looked at directly at enumeration.
                break;
            default:
                uart::printf("xhci: event type %u ignored\n", (uint32_t)trb->trb_type);
                break;
        }
    }

    if (any)
    {
        evt_ring_advance_erdp(hc);
        acknowledge_irq(hc, 0);
    }
}

// Everything the controller tells us about its own state. Printed when a
// command never completes: a command that goes out but produces no event
// means the controller is not reading our rings (or not writing events),
// which is a DMA problem, not a protocol one - and USBSTS says which.
static void dump_controller_state(xhci_controller* hc, const char* why)
{
    uint32_t usbsts = hc->op_regs->usbsts;

    uart::printf("xhci: %s: usbcmd=%x usbsts=%x%s%s%s%s%s%s\n",
                 why, hc->op_regs->usbcmd, usbsts,
                 (usbsts & XHCI_USBSTS_HCH) ? " halted" : "",
                 (usbsts & XHCI_USBSTS_HSE) ? " host-system-error" : "",
                 (usbsts & XHCI_USBSTS_HCE) ? " host-controller-error" : "",
                 (usbsts & XHCI_USBSTS_SRE) ? " save-restore-error" : "",
                 (usbsts & XHCI_USBSTS_EINT) ? " event-int" : "",
                 (usbsts & XHCI_USBSTS_CNR) ? " not-ready" : "");

    uint64_t crcr = read_mmio64(&hc->op_regs->crcr);
    uart::printf("xhci:   crcr=%llx%s dcbaap=%llx cmdring=%llx evtring=%llx\n",
                 crcr, (crcr & (1 << 3)) ? " running" : " stopped",
                 read_mmio64(&hc->op_regs->dcbaap),
                 (uint64_t)hc->cmd_ring.phys_base, (uint64_t)hc->evt_ring.phys_base);

    volatile xhci_interrupter_regs* ir = hc->evt_ring.interrupter;
    uart::printf("xhci:   erstba=%llx erdp=%llx deq=%u cycle=%u evt[0].ctl=%x\n",
                 read_mmio64(&ir->erstba), read_mmio64(&ir->erdp),
                 (uint32_t)hc->evt_ring.dequeue_ptr, (uint32_t)hc->evt_ring.cycle_bit,
                 hc->evt_ring.trbs[hc->evt_ring.dequeue_ptr].control);
}

// Send a command and wait for its completion. The result is a copy the
// controller does not write to; it stays valid until the next command.
static xhci_cmd_completion_trb_t* send_command(xhci_controller* hc, xhci_trb_t* cmd_trb, uint32_t timeout_ms)
{
    hc->cmd_done = false;
    hc->cmd_pending = hc->cmd_ring.phys_base + hc->cmd_ring.enqueue_ptr * sizeof(xhci_trb_t);
    cmd_ring_enqueue(&hc->cmd_ring, cmd_trb);
    ring_command_doorbell(hc);

    for (uint32_t waited = 0;; waited++)
    {
        process_events(hc);
        if (hc->cmd_done)
            return &hc->cmd_result;
        if (waited >= timeout_ms)
            break;
        delay_ms(1);
    }

    uart::printf("xhci: command timeout after %u ms\n", timeout_ms);
    dump_controller_state(hc, "cmd timeout");
    return nullptr;
}

// Before a transfer starts on `ring`: forget how the last one ended.
static void transfer_start(xhci_transfer_ring* ring)
{
    ring->done = false;
    ring->cc = 0;
    ring->residue = 0;
}

// Wait for the transfer started on `ring` to end. False on timeout;
// otherwise ring->cc and ring->residue say how it went.
static bool transfer_wait(xhci_controller* hc, xhci_transfer_ring* ring, uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited++)
    {
        process_events(hc);
        if (ring->done)
            return true;
        if (waited >= timeout_ms)
            return false;
        delay_ms(1);
    }
}

static bool transfer_ok(const xhci_transfer_ring* ring)
{
    return ring->cc == XHCI_TRB_COMPLETION_SUCCESS || ring->cc == XHCI_TRB_COMPLETION_SHORT_PACKET;
}
// Reset endpoint and set new dequeue pointer
static bool reset_endpoint(xhci_controller* hc, uint8_t slot_id, uint8_t dci, xhci_transfer_ring* ring)
{
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.control =
        (XHCI_TRB_TYPE_RESET_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: reset endpoint failed dci=%u\n", (uint32_t)dci);
        return false;
    }

    // Set TR Dequeue Pointer
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    uintptr_t new_dequeue = ring->phys_base + (ring->enqueue_ptr * sizeof(xhci_trb_t));
    cmd.parameter = (uint64_t)(new_dequeue | ring->cycle_bit);
    cmd.control = (XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24) |
                  ((uint32_t)dci << 16);

    cc = send_command(hc, &cmd, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: set TR dequeue ptr failed dci=%u\n", (uint32_t)dci);
        return false;
    }
    return true;
}

// Controller initialization helpers

static void parse_cap_regs(xhci_controller* hc)
{
    hc->cap_regs = (volatile xhci_cap_regs*)hc->base;

    hc->max_device_slots = XHCI_MAX_DEVICE_SLOTS(hc->cap_regs);
    if (hc->max_device_slots > XHCI_MAX_SLOTS)
        hc->max_device_slots = XHCI_MAX_SLOTS;
    hc->max_ports = XHCI_MAX_PORTS(hc->cap_regs);
    hc->max_scratchpad_bufs = XHCI_MAX_SCRATCHPAD_BUFFERS(hc->cap_regs);
    hc->xecp_offset = XHCI_XECP(hc->cap_regs) * sizeof(uint32_t);
    hc->ctx_entry_size = XHCI_CSZ(hc->cap_regs) ? 64 : 32;

    hc->op_regs = (volatile xhci_op_regs*)(hc->base + hc->cap_regs->caplength);
    hc->runtime_regs = (volatile xhci_runtime_regs*)(hc->base + hc->cap_regs->rtsoff);
    hc->doorbells = (volatile xhci_doorbell_reg*)(hc->base + hc->cap_regs->dboff);
}

static void read_supported_protocol(volatile uint32_t* cap, xhci_supported_protocol* out)
{
    uint32_t dw0 = cap[0];
    out->id = dw0 & 0xFF;
    out->next = (dw0 >> 8) & 0xFF;
    out->minor_rev = (dw0 >> 16) & 0xFF;
    out->major_rev = (dw0 >> 24) & 0xFF;
    out->name = cap[1];
    uint32_t dw2 = cap[2];
    out->compatible_port_offset = dw2 & 0xFF;
    out->compatible_port_count = (dw2 >> 8) & 0xFF;
    out->protocol_defined = (dw2 >> 16) & 0xFF;
    out->psic = (dw2 >> 24) & 0xFF;
    out->dword3 = cap[3];
}

static void parse_extended_capabilities(xhci_controller* hc)
{
    if (hc->xecp_offset == 0) return;
    volatile uint32_t* cap = (volatile uint32_t*)(hc->base + hc->xecp_offset);
    hc->usb3_port_count = 0;

    while (true)
    {
        uint32_t dw0 = *cap;
        uint8_t cap_id = dw0 & 0xFF;
        uint8_t next = (dw0 >> 8) & 0xFF;

        if (cap_id == XHCI_EXT_CAP_PROTOCOL)
        {
            xhci_supported_protocol proto;
            read_supported_protocol(cap, &proto);

            uint8_t first_port = proto.compatible_port_offset - 1;
            uint8_t port_count = proto.compatible_port_count;

            if (proto.major_rev == 3)
            {
                for (uint8_t i = 0; i < port_count && hc->usb3_port_count < XHCI_MAX_USB3_PORTS; i++)
                    hc->usb3_ports[hc->usb3_port_count++] = first_port + i;
            }
        }

        if (next == 0) break;
        cap = (volatile uint32_t*)((char*)cap + (next * sizeof(uint32_t)));
    }
}

static bool is_usb3_port(xhci_controller* hc, uint8_t port_num)
{
    for (uint8_t i = 0; i < hc->usb3_port_count; i++)
    {
        if (hc->usb3_ports[i] == port_num)
            return true;
    }
    return false;
}

static bool take_ownership_from_bios(xhci_controller* hc)
{
    if (hc->xecp_offset == 0)
        return true;
    volatile uint32_t* ecap = (volatile uint32_t*)(hc->base + hc->xecp_offset);

    while (true)
    {
        uint32_t val = *ecap;
        uint8_t cap_id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (cap_id == XHCI_LEGACY_SUPPORT_CAP_ID)
        {
            *ecap = val | XHCI_LEGACY_OS_OWNED;
            uint32_t timeout = 500;
            while ((*ecap & XHCI_LEGACY_BIOS_OWNED) && timeout > 0)
            {
                delay_ms(1);
                timeout--;
            }
            if (*ecap & XHCI_LEGACY_BIOS_OWNED)
            {
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

static bool reset_controller(xhci_controller* hc)
{
    hc->op_regs->usbcmd &= ~XHCI_USBCMD_RUN_STOP;
    uint32_t timeout = 200;
    while (!(hc->op_regs->usbsts & XHCI_USBSTS_HCH))
    {
        if (--timeout == 0)
            return false;
        delay_ms(1);
    }

    hc->op_regs->usbcmd |= XHCI_USBCMD_HCRESET;
    timeout = 1000;
    while ((hc->op_regs->usbcmd & XHCI_USBCMD_HCRESET) || (hc->op_regs->usbsts & XHCI_USBSTS_CNR))
    {
        if (--timeout == 0)
            return false;
        delay_ms(1);
    }
    delay_ms(50);
    return true;
}

// PAGESIZE is a bitmap: bit n set means the controller works in pages of
// 2^(n+12) bytes. Everything in the wild reports 4 KiB, but the scratchpad
// buffers have to match whatever this says, so read it rather than assume.
static uint32_t controller_page_size(xhci_controller* hc)
{
    uint32_t bits = hc->op_regs->pagesize & 0xFFFF;
    for (uint32_t n = 0; n < 16; n++)
    {
        if (bits & (1u << n))
            return 1u << (n + 12);
    }

    uart::printf("xhci: pagesize register is empty (%x), assuming 4 KiB\n", bits);
    return 4096;
}

static void setup_dcbaa(xhci_controller* hc)
{
    size_t dcbaa_size = sizeof(uint64_t) * (hc->max_device_slots + 1);
    hc->dcbaa = (uint64_t*)alloc_xhci_memory(dcbaa_size, XHCI_DCBAA_ALIGNMENT, XHCI_DCBAA_BOUNDARY);
    hc->dcbaa_virt = (uint64_t*)kmalloc(sizeof(uint64_t) * (hc->max_device_slots + 1));
    memory::memset((uint8_t*)hc->dcbaa_virt, 0, sizeof(uint64_t) * (hc->max_device_slots + 1));

    // Scratchpad buffers are the controller's own workspace, and it starts
    // using them the moment it runs - a wrong pointer here breaks everything
    // downstream with no error to show for it. QEMU asks for none, so this
    // path first runs on hardware; Alder Lake's PCH asks for 34.
    if (hc->max_scratchpad_bufs > 0)
    {
        // Their size and alignment is the page size the *controller* uses,
        // which is its own register, not the CPU's 4 KiB.
        uint32_t page_size = controller_page_size(hc);

        uint64_t* sp_array = (uint64_t*)alloc_xhci_memory(hc->max_scratchpad_bufs * sizeof(uint64_t),
                                                          XHCI_DCBAA_ALIGNMENT, page_size);
        for (uint32_t i = 0; i < hc->max_scratchpad_bufs; i++)
        {
            void* sp_page = alloc_xhci_memory(page_size, page_size, page_size);
            sp_array[i] = xhci_virt_to_phys(sp_page);
        }
        hc->dcbaa[0] = xhci_virt_to_phys(sp_array);
        hc->dcbaa_virt[0] = (uint64_t)sp_array;

        uart::printf("xhci: %u scratchpad buffer(s) of %u bytes, array at %llx\n",
                     (uint32_t)hc->max_scratchpad_bufs, page_size, (uint64_t)hc->dcbaa[0]);
    }

    write_mmio64(&hc->op_regs->dcbaap, xhci_virt_to_phys(hc->dcbaa));
}

static void configure_operational_regs(xhci_controller* hc)
{
    hc->op_regs->dnctrl = 0xFFFF;
    hc->op_regs->config = (uint32_t)hc->max_device_slots;
    setup_dcbaa(hc);

    size_t rings_size = sizeof(xhci_transfer_ring*) * (hc->max_device_slots + 1) * XHCI_MAX_DCI;
    hc->rings = (xhci_transfer_ring**)kmalloc(rings_size);
    memory::memset((uint8_t*)hc->rings, 0, rings_size);

    cmd_ring_init(&hc->cmd_ring, XHCI_COMMAND_RING_TRB_COUNT);
    write_mmio64(&hc->op_regs->crcr, hc->cmd_ring.phys_base | hc->cmd_ring.cycle_bit);
}

static void configure_runtime_regs(xhci_controller* hc)
{
    volatile xhci_interrupter_regs* ir = &hc->runtime_regs->ir[0];
    ir->iman |= XHCI_IMAN_INTERRUPT_ENABLE;
    evt_ring_init(&hc->evt_ring, XHCI_EVENT_RING_TRB_COUNT, ir);
    acknowledge_irq(hc, 0);
}

static bool start_controller(xhci_controller* hc)
{
    uint32_t usbcmd = hc->op_regs->usbcmd;
    usbcmd |= XHCI_USBCMD_RUN_STOP | XHCI_USBCMD_INTERRUPTER_ENABLE | XHCI_USBCMD_HOSTSYS_ERR_EN;
    hc->op_regs->usbcmd = usbcmd;

    uint32_t timeout = 1000;
    while (hc->op_regs->usbsts & XHCI_USBSTS_HCH)
    {
        if (--timeout == 0)
            return false;
        delay_ms(1);
    }
    if (hc->op_regs->usbsts & XHCI_USBSTS_CNR)
        return false;

    return true;
}

// Port operations

static xhci_portsc read_portsc(xhci_controller* hc, uint8_t port)
{
    uint64_t addr = (uint64_t)hc->op_regs + 0x400 + (0x10 * port);
    xhci_portsc reg;
    reg.raw = *(volatile uint32_t*)addr;
    return reg;
}

static void write_portsc(xhci_controller* hc, xhci_portsc reg, uint8_t port)
{
    uint64_t addr = (uint64_t)hc->op_regs + 0x400 + (0x10 * port);
    *(volatile uint32_t*)addr = reg.raw;
}

// Read-modify-write of PORTSC, minus the bits that a plain write-back would
// destroy: PED disables the port when a 1 is written to it, and the change
// bits (CSC/PEC/WRC/OCC/PRC/PLC/CEC) are write-1-to-clear, so carrying the
// value just read back into the register silently acknowledges events we
// have not handled. Only the callers that mean to clear a change bit use
// write_portsc() directly.
static void write_portsc_preserving(xhci_controller* hc, xhci_portsc reg, uint8_t port)
{
    reg.ped = 0;
    reg.csc = 0;
    reg.pec = 0;
    reg.wrc = 0;
    reg.occ = 0;
    reg.prc = 0;
    reg.plc = 0;
    reg.cec = 0;
    write_portsc(hc, reg, port);
}

// After a host controller reset, a controller with Port Power Control brings
// its ports up unpowered, and an unpowered port reports CCS = 0 no matter
// what is plugged into it. QEMU's xHCI leaves PP set, so a port scan that
// only looks at CCS works there and finds nothing at all on hardware - down
// to a laptop's internal webcam and Bluetooth.
static void power_on_all_ports(xhci_controller* hc)
{
    if (!XHCI_PPC(hc->cap_regs))
        return;

    bool powered_any = false;
    for (uint8_t i = 0; i < hc->max_ports; i++)
    {
        xhci_portsc portsc = read_portsc(hc, i);
        if (portsc.pp)
            continue;

        portsc.pp = 1;
        write_portsc_preserving(hc, portsc, i);
        powered_any = true;
    }

    if (!powered_any)
        return;

    delay_ms(XHCI_PORT_POWER_SETTLE_MS);

    for (uint8_t i = 0; i < hc->max_ports; i++)
    {
        if (read_portsc(hc, i).pp == 0)
            uart::printf("xhci: port %u failed to power on\n", (uint32_t)i);
    }
}

// Wait for the ports to report what is attached. A USB3 link trains and a
// USB2 device debounces over tens to hundreds of milliseconds after power is
// applied; sampling CCS once, right after the controller starts, races that
// on every real machine.
static uint8_t wait_for_port_connections(xhci_controller* hc)
{
    uint32_t waited = 0;
    uint8_t connected = 0;

    while (waited < XHCI_PORT_SCAN_TIMEOUT_MS)
    {
        connected = 0;
        for (uint8_t i = 0; i < hc->max_ports; i++)
        {
            if (read_portsc(hc, i).ccs)
                connected++;
        }

        if (connected > 0)
            break;

        delay_ms(XHCI_PORT_POLL_INTERVAL_MS);
        waited += XHCI_PORT_POLL_INTERVAL_MS;
    }

    // Let the slower ports catch up with the first one that answered, so a
    // single pass over the port list sees all of them.
    delay_ms(XHCI_PORT_DEBOUNCE_MS);

    connected = 0;
    for (uint8_t i = 0; i < hc->max_ports; i++)
    {
        if (read_portsc(hc, i).ccs)
            connected++;
    }

    uart::printf("xhci: %u port(s) connected after %u ms\n",
                 (uint32_t)connected, waited + XHCI_PORT_DEBOUNCE_MS);
    return connected;
}

static bool reset_port(xhci_controller* hc, uint8_t port_num)
{
    xhci_portsc portsc = read_portsc(hc, port_num);
    bool usb3 = is_usb3_port(hc, port_num);

    // Power came on in power_on_all_ports() before the scan; a port that is
    // still unpowered here is broken, not merely idle.
    if (portsc.pp == 0)
    {
        uart::printf("xhci: port %u is not powered\n", (uint32_t)port_num);
        return false;
    }

    // Clear change bits
    portsc.csc = 1;
    portsc.pec = 1;
    portsc.prc = 1;
    write_portsc(hc, portsc, port_num);

    // Initiate reset. PED stays out of the written value: writing a 1 there
    // disables the port, and firmware may well have left it enabled.
    portsc = read_portsc(hc, port_num);
    portsc.ped = 0;
    if (usb3)
        portsc.wpr = 1;
    else
        portsc.pr = 1;
    write_portsc(hc, portsc, port_num);

    // Wait for reset completion
    uint32_t timeout = 100;
    while (timeout > 0)
    {
        portsc = read_portsc(hc, port_num);
        if ((usb3 && portsc.wrc) || (!usb3 && portsc.prc))
            break;
        timeout--;
        delay_ms(1);
    }
    if (timeout == 0)
    {
        uart::printf("xhci: port %u reset timed out\n", (uint32_t)port_num);
        return false;
    }

    delay_ms(3);

    // Clear reset completion bits, preserve PED
    portsc = read_portsc(hc, port_num);
    portsc.prc = 1;
    portsc.wrc = 1;
    portsc.csc = 1;
    portsc.pec = 1;
    portsc.ped = 0;
    write_portsc(hc, portsc, port_num);
    delay_ms(3);

    portsc = read_portsc(hc, port_num);
    if (portsc.ped == 0)
    {
        uart::printf("xhci: port %u not enabled after reset\n", (uint32_t)port_num);
        return false;
    }
    return true;
}

// Device setup

static uint8_t enable_device_slot(xhci_controller* hc)
{
    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.trb_type = XHCI_TRB_TYPE_ENABLE_SLOT_CMD;

    xhci_cmd_completion_trb_t* cc = send_command(hc, &trb, 200);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
        return 0;

    return cc->slot_id;
}

// A device context is 32 entries (slot + EP0 + 30 endpoints); an input
// context prepends the input control context, so 33.
#define XHCI_DEVICE_CTX_ENTRIES 32
#define XHCI_INPUT_CTX_ENTRIES  33

static xhci_input_control_context* input_control_ctx(void* input_ctx)
{
    return (xhci_input_control_context*)input_ctx;
}

// The device context embedded in an input context starts one entry in, so a
// Device Context Index addresses entry 1 + dci there and entry dci in a
// device context proper. DCI 1 is the control endpoint.
static xhci_slot_context* input_slot_ctx(xhci_controller* hc, void* input_ctx)
{
    return (xhci_slot_context*)((uint8_t*)input_ctx + hc->ctx_entry_size);
}

static xhci_endpoint_context* input_ep_ctx(xhci_controller* hc, void* input_ctx, uint8_t dci)
{
    return (xhci_endpoint_context*)((uint8_t*)input_ctx + hc->ctx_entry_size * (1 + dci));
}

static xhci_slot_context* device_slot_ctx(void* device_ctx)
{
    return (xhci_slot_context*)device_ctx;
}

static bool create_device_context(xhci_controller* hc, uint8_t slot_id)
{
    void* ctx = alloc_xhci_memory(hc->ctx_entry_size * XHCI_DEVICE_CTX_ENTRIES,
                                  XHCI_DEVICE_CTX_ALIGNMENT, XHCI_DEVICE_CTX_BOUNDARY);
    if (!ctx)
        return false;

    hc->dcbaa[slot_id] = xhci_virt_to_phys(ctx);
    hc->dcbaa_virt[slot_id] = (uint64_t)ctx;
    return true;
}

static void* alloc_input_context(xhci_controller* hc)
{
    return alloc_xhci_memory(hc->ctx_entry_size * XHCI_INPUT_CTX_ENTRIES,
                             XHCI_INPUT_CTX_ALIGNMENT, XHCI_INPUT_CTX_BOUNDARY);
}

static bool evaluate_context(xhci_controller* hc, uint8_t slot_id, uint16_t new_max_packet_size)
{
    void* input_ctx = alloc_input_context(hc);
    if (!input_ctx)
        return false;

    input_control_ctx(input_ctx)->add_flags = (1 << 1);
    input_ep_ctx(hc, input_ctx, 1)->max_packet_size = new_max_packet_size;

    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 200);
    // Done with once the command completes; one that never did may still
    // be read by the controller, so it is left alone then.
    if (cc)
        free_xhci_memory(input_ctx);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: evaluate context failed slot=%u\n", (uint32_t)slot_id);
        return false;
    }
    return true;
}

// Controller selection

// Halt a controller we are about to walk away from: with R/S clear it stops
// fetching TRBs, so the rings and contexts allocated for it become inert.
static void stop_controller(xhci_controller* hc)
{
    if (!hc->op_regs)
        return;

    hc->op_regs->usbcmd &= ~(uint32_t)(XHCI_USBCMD_RUN_STOP | XHCI_USBCMD_INTERRUPTER_ENABLE);
    uint32_t timeout = 200;
    while (!(hc->op_regs->usbsts & XHCI_USBSTS_HCH) && timeout > 0)
    {
        delay_ms(1);
        timeout--;
    }
}


// Transfers

// The 8-byte setup packet as the Setup Stage TRB carries it (immediate data).
static uint64_t setup_as_parameter(const uint8_t* setup)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | setup[i];
    return v;
}

static bool control_transfer(xhci_controller* hc, uint8_t slot, xhci_transfer_ring* ring, const uint8_t* setup,
                             uintptr_t data_phys, uint16_t length, bool in, uint32_t timeout_ms)
{
    transfer_start(ring);

    // Setup Stage. TRT: 0 no data stage, 2 OUT data, 3 IN data.
    uint32_t trt = length == 0 ? 0 : (in ? 3 : 2);
    xhci_trb_t setup_trb;
    memory::memset((uint8_t*)&setup_trb, 0, sizeof(xhci_trb_t));
    setup_trb.parameter = setup_as_parameter(setup);
    setup_trb.status = 8;
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 6) // IDT
                        | (trt << 16);
    transfer_ring_enqueue(ring, &setup_trb);

    if (length > 0)
    {
        xhci_trb_t data_trb;
        memory::memset((uint8_t*)&data_trb, 0, sizeof(xhci_trb_t));
        data_trb.parameter = (uint64_t)data_phys;
        data_trb.status = length;
        data_trb.control = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT)
                           | (in ? (1 << 16) : 0)   // DIR
                           | (in ? (1 << 2) : 0);   // ISP: a short reply is reported, with how short
        transfer_ring_enqueue(ring, &data_trb);
    }

    // Status Stage, in the direction opposite to the data (IN without data).
    xhci_trb_t status_trb;
    memory::memset((uint8_t*)&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 5)  // IOC
                         | ((length == 0 || !in) ? (1 << 16) : 0);                      // DIR=IN
    transfer_ring_enqueue(ring, &status_trb);

    ring_doorbell(hc, slot, XHCI_DOORBELL_TARGET_CONTROL_EP);
    return transfer_wait(hc, ring, timeout_ms);
}

static bool normal_transfer(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring,
                            uintptr_t data_phys, uint32_t length, uint32_t timeout_ms)
{
    transfer_start(ring);

    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)data_phys;
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | (1 << 5)    // IOC
                  | (1 << 2);                                                   // ISP
    transfer_ring_enqueue(ring, &trb);

    ring_doorbell(hc, slot, dci);
    return transfer_wait(hc, ring, timeout_ms);
}

// Device slots

static bool address_device(xhci_controller* hc, uint8_t slot_id, uint8_t port, uint8_t speed,
                           uint16_t max_packet, xhci_transfer_ring* ep0)
{
    if (!create_device_context(hc, slot_id))
        return false;

    transfer_ring_init(ep0, XHCI_TRANSFER_RING_TRB_COUNT);
    set_ring(hc, slot_id, 1, ep0);

    void* input_ctx = alloc_input_context(hc);
    if (!input_ctx)
        return false;

    input_control_ctx(input_ctx)->add_flags = (1 << 0) | (1 << 1);
    input_control_ctx(input_ctx)->drop_flags = 0;

    xhci_slot_context* slot = input_slot_ctx(hc, input_ctx);
    slot->route_string = 0;
    slot->speed = speed;
    slot->context_entries = 1;
    slot->root_hub_port_num = port + 1;

    xhci_endpoint_context* ep0_ctx = input_ep_ctx(hc, input_ctx, 1);
    ep0_ctx->endpoint_type = XHCI_EP_TYPE_CONTROL_BIDIR;
    ep0_ctx->max_packet_size = max_packet;
    ep0_ctx->max_burst_size = 0;
    ep0_ctx->error_count = 3;
    ep0_ctx->average_trb_length = 8;
    ep0_ctx->transfer_ring_dequeue_ptr = ep0->phys_base | 1;

    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 500);
    // Done with once the command completes; one that never did may still
    // be read by the controller, so it is left alone then.
    if (cc)
        free_xhci_memory(input_ctx);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: address device failed port=%u code=%u\n", (uint32_t)port,
                     cc ? (uint32_t)cc->completion_code : 0);
        return false;
    }
    return true;
}

static bool configure_endpoints(xhci_controller* hc, uint8_t slot_id, xhci_ep_config* eps, uint8_t count)
{
    void* input_ctx = alloc_input_context(hc);
    if (!input_ctx)
        return false;

    // The slot context comes along (A0) because Context Entries grows to
    // the highest DCI in use.
    void* out_ctx = (void*)hc->dcbaa_virt[slot_id];
    *input_slot_ctx(hc, input_ctx) = *device_slot_ctx(out_ctx);
    uint8_t max_dci = device_slot_ctx(out_ctx)->context_entries;

    uint32_t add = 1 << 0;
    for (uint8_t i = 0; i < count; i++)
    {
        xhci_ep_config* e = &eps[i];
        transfer_ring_init(e->ring, XHCI_TRANSFER_RING_TRB_COUNT);

        xhci_endpoint_context* ctx = input_ep_ctx(hc, input_ctx, e->dci);
        ctx->endpoint_type = e->type;
        ctx->max_packet_size = e->max_packet;
        ctx->max_burst_size = 0;
        ctx->error_count = 3;
        ctx->interval = e->interval;
        bool periodic = e->type == XHCI_EP_TYPE_INTERRUPT_IN || e->type == XHCI_EP_TYPE_INTERRUPT_OUT;
        ctx->average_trb_length = periodic ? e->max_packet : 1024;
        ctx->max_esit_payload_lo = periodic ? e->max_packet : 0;
        ctx->transfer_ring_dequeue_ptr = e->ring->phys_base | 1;

        add |= 1u << e->dci;
        if (e->dci > max_dci)
            max_dci = e->dci;
    }
    input_slot_ctx(hc, input_ctx)->context_entries = max_dci;
    input_control_ctx(input_ctx)->add_flags = add;
    input_control_ctx(input_ctx)->drop_flags = 0;

    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 500);
    if (cc)
        free_xhci_memory(input_ctx);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: configure endpoint failed slot=%u code=%u\n", (uint32_t)slot_id,
                     cc ? (uint32_t)cc->completion_code : 0);
        return false;
    }

    for (uint8_t i = 0; i < count; i++)
        set_ring(hc, slot_id, eps[i].dci, eps[i].ring);
    return true;
}

// Bring up one controller, up to the point where its ports show what is
// attached. Everything it had before is forgotten. The DMA allocations it
// made are not reclaimed - alloc_xhci_memory has no size-tracking free list
// and this runs a handful of times at boot - but nothing points at them any
// more.
static bool start(xhci_controller* hc)
{
    PCIDevice* dev = hc->pci;
    memory::memset((uint8_t*)hc, 0, sizeof(*hc));
    hc->pci = dev;
    hc->ctx_entry_size = 32;

    pci::enable_device(dev);

    PCIBar bar = pci::get_bar(dev, 0);
    if (!bar.valid || bar.is_io)
    {
        uart::printf("xhci: %u:%u.%u: invalid BAR0\n",
                     (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);
        return false;
    }

    uint64_t bar_size = get_bar_size_64(dev, 0);
    hc->base = xhci_map_mmio(bar.base, bar_size);
    if (!hc->base)
    {
        uart::printf("xhci: %u:%u.%u: MMIO map failed\n",
                     (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);
        return false;
    }

    parse_cap_regs(hc);
    parse_extended_capabilities(hc);
    uart::printf("xhci: %u:%u.%u: %u ports, %u slots, ctx=%u, scratchpad=%u, bar=%llx\n",
                 (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function,
                 (uint32_t)hc->max_ports, (uint32_t)hc->max_device_slots,
                 hc->ctx_entry_size, (uint32_t)hc->max_scratchpad_bufs, (uint64_t)bar.base);

    if (!take_ownership_from_bios(hc))
        return false;
    if (!reset_controller(hc))
    {
        uart::printf("xhci: %u:%u.%u: reset failed\n",
                     (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);
        return false;
    }
    configure_operational_regs(hc);
    configure_runtime_regs(hc);
    if (!start_controller(hc))
    {
        uart::printf("xhci: %u:%u.%u: start failed\n",
                     (uint32_t)dev->bus, (uint32_t)dev->device, (uint32_t)dev->function);
        return false;
    }

    process_events(hc);

    uart::printf("xhci: dma: cmdring=%llx evtring=%llx dcbaa=%llx\n",
                 (uint64_t)hc->cmd_ring.phys_base, (uint64_t)hc->evt_ring.phys_base,
                 (uint64_t)xhci_virt_to_phys(hc->dcbaa));
    dump_controller_state(hc, "after start");

    power_on_all_ports(hc);
    wait_for_port_connections(hc);
    return true;
}

// The controller as usb.cpp sees it (xhci.h).

namespace xhci
{
    uint8_t find_controllers()
    {
        controller_count = 0;

        // Every xHCI controller. A desktop board usually has one (the PCH at
        // 00:14.0), but a mobile chipset commonly adds a second one for its
        // USB4/Thunderbolt ports - and that one sits at a *lower* device
        // number (00:0d.0 on Alder Lake-P), so "the first xHCI on the bus"
        // is the controller with no user-facing ports on exactly the
        // machines where that matters.
        for (uint32_t i = 0; i < pci::device_count() &&
                             controller_count < MAX_XHCI_CONTROLLERS; i++)
        {
            PCIDevice* d = pci::get_by_id(i);
            if (!d || !d->valid)
                continue;
            if (d->class_code != PCI_CLASS_SERIAL || d->subclass != 0x03 ||
                d->prog_if != 0x30)
                continue;
            memory::memset((uint8_t*)&controllers[controller_count], 0, sizeof(xhci_controller));
            controllers[controller_count++].pci = d;
        }
        return controller_count;
    }

    xhci_controller* controller(uint8_t index)
    {
        return index < controller_count ? &controllers[index] : nullptr;
    }

    PCIDevice* pci_device(xhci_controller* hc)
    {
        return hc->pci;
    }

    bool start(xhci_controller* hc)
    {
        return ::start(hc);
    }

    void stop(xhci_controller* hc)
    {
        stop_controller(hc);
    }

    uint8_t port_count(xhci_controller* hc)
    {
        return hc->op_regs ? hc->max_ports : 0;
    }

    uint32_t port_status(xhci_controller* hc, uint8_t port)
    {
        if (!hc->op_regs || port >= hc->max_ports)
            return 0;
        return read_portsc(hc, port).raw;
    }

    bool port_is_usb3(xhci_controller* hc, uint8_t port)
    {
        return is_usb3_port(hc, port);
    }

    bool port_connected(xhci_controller* hc, uint8_t port)
    {
        return read_portsc(hc, port).ccs;
    }

    bool reset_port(xhci_controller* hc, uint8_t port)
    {
        return ::reset_port(hc, port);
    }

    uint8_t port_speed(xhci_controller* hc, uint8_t port)
    {
        return read_portsc(hc, port).port_speed;
    }

    uint32_t context_entry_size(xhci_controller* hc)
    {
        return hc->ctx_entry_size;
    }

    uint8_t enable_slot(xhci_controller* hc)
    {
        return enable_device_slot(hc);
    }

    bool address_device(xhci_controller* hc, uint8_t slot, uint8_t port, uint8_t speed,
                        uint16_t max_packet, xhci_transfer_ring* ep0)
    {
        return ::address_device(hc, slot, port, speed, max_packet, ep0);
    }

    bool set_ep0_max_packet(xhci_controller* hc, uint8_t slot, uint16_t max_packet)
    {
        return evaluate_context(hc, slot, max_packet);
    }

    bool configure_endpoints(xhci_controller* hc, uint8_t slot, xhci_ep_config* eps, uint8_t count)
    {
        return ::configure_endpoints(hc, slot, eps, count);
    }

    bool reset_endpoint(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring)
    {
        return ::reset_endpoint(hc, slot, dci, ring);
    }

    bool control(xhci_controller* hc, uint8_t slot, xhci_transfer_ring* ep0, const uint8_t* setup,
                 uintptr_t data_phys, uint16_t length, bool in, uint32_t timeout_ms)
    {
        return control_transfer(hc, slot, ep0, setup, data_phys, length, in, timeout_ms);
    }

    bool normal(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring,
                uintptr_t data_phys, uint32_t length, uint32_t timeout_ms)
    {
        return normal_transfer(hc, slot, dci, ring, data_phys, length, timeout_ms);
    }

    bool completed_ok(const xhci_transfer_ring* ring)
    {
        return transfer_ok(ring);
    }

    const char* completion_code_str(uint8_t code)
    {
        return ::completion_code_str(code);
    }

    void* dma_alloc(size_t size, size_t alignment, size_t boundary)
    {
        return alloc_xhci_memory(size, alignment, boundary);
    }

    void dma_free(void* ptr)
    {
        free_xhci_memory(ptr);
    }

    uintptr_t phys(void* vaddr)
    {
        return xhci_virt_to_phys(vaddr);
    }

    void delay_ms(uint32_t ms)
    {
        ::delay_ms(ms);
    }
}
