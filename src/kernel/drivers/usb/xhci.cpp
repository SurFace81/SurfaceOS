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

static const char* usb_speed_str(uint8_t speed)
{
    switch (speed)
    {
        case 1: return "Full (12 Mb/s)";
        case 2: return "Low (1.5 Mb/s)";
        case 3: return "High (480 Mb/s)";
        case 4: return "SS (5 Gb/s)";
        case 5: return "SS+ (10 Gb/s)";
        default: return "Unknown";
    }
}

static const char* usb_class_name(uint8_t cls)
{
    switch (cls)
    {
        case 0x00: return "Composite";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "Smart Card";
        case 0x0E: return "Video";
        case 0x0F: return "Healthcare";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "App Specific";
        case 0xFF: return "Vendor Specific";
        default:   return "Unknown";
    }
}

static uint16_t max_packet_size_for_speed(uint8_t speed)
{
    switch (speed)
    {
        case 1:
            return 64;
        case 2:
            return 8;
        case 3:
            return 64;
        case 4:
            return 512;
        case 5:
            return 512;
        default:
            return 64;
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

struct xhci_transfer_ring
{
    xhci_trb_t* trbs;
    uintptr_t phys_base;
    size_t max_trb_count;
    size_t enqueue_ptr;
    uint8_t cycle_bit;

    // How the transfer last started on this ring ended, filled in by
    // process_events(): `done` once its final event arrived, with that
    // event's completion code, and the bytes a short packet left untouched.
    volatile bool done;
    uint8_t cc;
    uint32_t residue;
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

// Internal per-device mass storage state
struct xhci_controller;

struct usb_mass_storage_dev
{
    xhci_controller* hc;
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
    // SYNCHRONIZE CACHE was rejected as an unknown command: the device has
    // no cache it lets us flush, so flushes are skipped from then on.
    bool no_sync_cache;
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
    xhci_transfer_ring ep0_rings[XHCI_MAX_SLOTS + 1];   // by slot ID

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
// The controller we drive; lsusb/boot diagnostics report it on machines
// with no serial port.
static xhci_controller* active = nullptr;

#define MAX_MASS_STORAGE_DEVS 8
static usb_mass_storage_dev mass_storage_devs[MAX_MASS_STORAGE_DEVS];
static uint8_t mass_storage_count = 0;

// Cached capacity per mass storage device
static uint32_t msd_block_size[MAX_MASS_STORAGE_DEVS];
static uint32_t msd_last_lba[MAX_MASS_STORAGE_DEVS];
static bool msd_capacity_cached[MAX_MASS_STORAGE_DEVS];

// One reusable DMA bounce buffer per device, USB_MAX_XFER_BYTES long.
// Allocating per request made every FAT sector read a kmalloc + a
// DMA-capable carve-out; on real USB sticks that dominated small-transfer
// latency.
static uint8_t* msd_dma_buf[MAX_MASS_STORAGE_DEVS];
static uintptr_t msd_dma_phys[MAX_MASS_STORAGE_DEVS];
static uint32_t msd_dma_size[MAX_MASS_STORAGE_DEVS];

// BOT shared buffers
static usb_cbw* shared_cbw = nullptr;
static usb_csw* shared_csw = nullptr;
static uintptr_t shared_cbw_phys = 0;
static uintptr_t shared_csw_phys = 0;
static uint32_t bot_tag = 1;

// Public API device registry
#define MAX_USB_DEVICES 64
static usb_device_info device_infos[MAX_USB_DEVICES];
static uint8_t device_count = 0;

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

static bool transfer_ok(xhci_transfer_ring* ring)
{
    return ring->cc == XHCI_TRB_COMPLETION_SUCCESS || ring->cc == XHCI_TRB_COMPLETION_SHORT_PACKET;
}

// Control transfers

static sint32_t control_transfer_in(xhci_controller* hc, uint8_t slot_id, uint8_t* setup_packet, void* buffer, uintptr_t buffer_phys,
                                    uint16_t data_length)
{
    xhci_transfer_ring* ring = &hc->ep0_rings[slot_id];
    transfer_start(ring);

    // Setup Stage TRB
    xhci_trb_t setup_trb;
    memory::memset((uint8_t*)&setup_trb, 0, sizeof(xhci_trb_t));
    setup_trb.parameter = (uint64_t)setup_packet[0] | ((uint64_t)setup_packet[1] << 8) |
                          ((uint64_t)setup_packet[2] << 16) | ((uint64_t)setup_packet[3] << 24) |
                          ((uint64_t)setup_packet[4] << 32) | ((uint64_t)setup_packet[5] << 40) |
                          ((uint64_t)setup_packet[6] << 48) | ((uint64_t)setup_packet[7] << 56);
    setup_trb.status = 8;
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 6) // IDT
                        | (3 << 16);                                                  // TRT=3 (IN data stage)
    transfer_ring_enqueue(ring, &setup_trb);

    // Data Stage TRB
    xhci_trb_t data_trb;
    memory::memset((uint8_t*)&data_trb, 0, sizeof(xhci_trb_t));
    data_trb.parameter = (uint64_t)buffer_phys;
    data_trb.status = data_length;
    data_trb.control = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 16) // DIR=IN
                       | (1 << 2);   // ISP: a short reply is reported, with how short
    transfer_ring_enqueue(ring, &data_trb);

    // Status Stage TRB
    xhci_trb_t status_trb;
    memory::memset((uint8_t*)&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 5); // IOC
    transfer_ring_enqueue(ring, &status_trb);

    ring_doorbell(hc, slot_id, XHCI_DOORBELL_TARGET_CONTROL_EP);

    if (!transfer_wait(hc, ring, 500))
    {
        uart::printf("xhci: control IN timeout slot=%u\n", (uint32_t)slot_id);
        return -1;
    }

    if (!transfer_ok(ring))
    {
        uart::printf("xhci: control IN failed slot=%u code=%u (%s)\n", (uint32_t)slot_id,
                     (uint32_t)ring->cc, completion_code_str(ring->cc));
        return -1;
    }

    return (sint32_t)data_length - (sint32_t)ring->residue;
}

static bool control_transfer_no_data(xhci_controller* hc, uint8_t slot_id, uint8_t* setup_packet)
{
    xhci_transfer_ring* ring = &hc->ep0_rings[slot_id];
    transfer_start(ring);

    // Setup Stage TRB, TRT=0 (no data)
    xhci_trb_t setup_trb;
    memory::memset((uint8_t*)&setup_trb, 0, sizeof(xhci_trb_t));
    setup_trb.parameter = (uint64_t)setup_packet[0] | ((uint64_t)setup_packet[1] << 8) |
                          ((uint64_t)setup_packet[2] << 16) | ((uint64_t)setup_packet[3] << 24) |
                          ((uint64_t)setup_packet[4] << 32) | ((uint64_t)setup_packet[5] << 40) |
                          ((uint64_t)setup_packet[6] << 48) | ((uint64_t)setup_packet[7] << 56);
    setup_trb.status = 8;
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 6); // IDT, TRT=0
    transfer_ring_enqueue(ring, &setup_trb);

    // Status Stage TRB, DIR=IN since no data stage
    xhci_trb_t status_trb;
    memory::memset((uint8_t*)&status_trb, 0, sizeof(xhci_trb_t));
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | (1 << 5) // IOC
                         | (1 << 16);                                                   // DIR=IN
    transfer_ring_enqueue(ring, &status_trb);

    ring_doorbell(hc, slot_id, XHCI_DOORBELL_TARGET_CONTROL_EP);

    if (!transfer_wait(hc, ring, 500) || ring->cc != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: control no-data failed slot=%u\n", (uint32_t)slot_id);
        return false;
    }
    return true;
}

// Bulk transfers

static bool bulk_transfer_out(usb_mass_storage_dev* msd, void* data, uintptr_t data_phys, uint32_t length)
{
    xhci_controller* hc = msd->hc;
    xhci_transfer_ring* ring = &msd->bulk_out_ring;
    uint8_t out_dci = (msd->bulk_out_ep & 0x0F) * 2;
    transfer_start(ring);

    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)data_phys;
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | (1 << 5);
    transfer_ring_enqueue(ring, &trb);

    ring_doorbell(hc, msd->slot_id, out_dci);

    if (!transfer_wait(hc, ring, 2000))
    {
        uart::printf("xhci: bulk OUT timeout\n");
        return false;
    }
    if (!transfer_ok(ring))
    {
        uart::printf("xhci: bulk OUT failed code=%u (%s)\n", (uint32_t)ring->cc,
                     completion_code_str(ring->cc));
        return false;
    }
    return true;
}

// Clear STALL on an endpoint
static bool clear_endpoint_halt(xhci_controller* hc, uint8_t slot_id, uint8_t endpoint_address)
{
    uint8_t setup[8];
    setup[0] = 0x02;
    setup[1] = 0x01;
    setup[2] = 0x00;
    setup[3] = 0x00;
    setup[4] = endpoint_address;
    setup[5] = 0x00;
    setup[6] = 0x00;
    setup[7] = 0x00;
    return control_transfer_no_data(hc, slot_id, setup);
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

static bool recover_from_stall(usb_mass_storage_dev* msd, uint8_t endpoint_address)
{
    xhci_controller* hc = msd->hc;
    uint8_t ep_num = endpoint_address & 0x0F;
    bool is_in = (endpoint_address & 0x80) != 0;
    uint8_t dci = ep_num * 2 + (is_in ? 1 : 0);
    xhci_transfer_ring* ring = is_in ? &msd->bulk_in_ring : &msd->bulk_out_ring;

    uart::printf("xhci: recovering from STALL on ep=0x%x\n", (uint32_t)endpoint_address);

    if (!clear_endpoint_halt(hc, msd->slot_id, endpoint_address))
        return false;
    if (!reset_endpoint(hc, msd->slot_id, dci, ring))
        return false;
    return true;
}

static sint32_t bulk_transfer_in(usb_mass_storage_dev* msd, void* data, uintptr_t data_phys, uint32_t length)
{
    xhci_controller* hc = msd->hc;
    xhci_transfer_ring* ring = &msd->bulk_in_ring;
    uint8_t in_dci = (msd->bulk_in_ep & 0x0F) * 2 + 1;
    transfer_start(ring);

    xhci_trb_t trb;
    memory::memset((uint8_t*)&trb, 0, sizeof(xhci_trb_t));
    trb.parameter = (uint64_t)data_phys;
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | (1 << 5);
    transfer_ring_enqueue(ring, &trb);

    ring_doorbell(hc, msd->slot_id, in_dci);

    if (!transfer_wait(hc, ring, 2000))
    {
        uart::printf("xhci: bulk IN timeout\n");
        return -1;
    }
    if (ring->cc == XHCI_TRB_COMPLETION_STALL)
    {
        recover_from_stall(msd, msd->bulk_in_ep);
        return -2;
    }
    if (!transfer_ok(ring))
    {
        uart::printf("xhci: bulk IN failed code=%u (%s)\n", (uint32_t)ring->cc,
                     completion_code_str(ring->cc));
        return -1;
    }
    return (sint32_t)length - (sint32_t)ring->residue;
}

// BOT (Bulk-Only Transport) / SCSI layer

static void bot_init_buffers()
{
    if (!shared_cbw)
    {
        shared_cbw = (usb_cbw*)alloc_xhci_memory(sizeof(usb_cbw), 64, 4096);
        shared_cbw_phys = xhci_virt_to_phys(shared_cbw);
        shared_csw = (usb_csw*)alloc_xhci_memory(sizeof(usb_csw), 64, 4096);
        shared_csw_phys = xhci_virt_to_phys(shared_csw);
    }
}

static sint32_t bot_scsi_command(usb_mass_storage_dev* msd, uint8_t* scsi_cmd, uint8_t scsi_cmd_len, void* data_buf,
                                 uintptr_t data_phys, uint32_t data_length, uint8_t direction)
{
    bot_init_buffers();

    // Build CBW
    memory::memset((uint8_t*)shared_cbw, 0, sizeof(usb_cbw));
    shared_cbw->dCBWSignature = USB_CBW_SIGNATURE;
    shared_cbw->dCBWTag = bot_tag++;
    shared_cbw->dCBWDataTransferLength = data_length;
    shared_cbw->bmCBWFlags = direction;
    shared_cbw->bCBWLUN = 0;
    shared_cbw->bCBWCBLength = scsi_cmd_len;
    memory::memcpy(shared_cbw->CBWCB, scsi_cmd, scsi_cmd_len);

    // Command phase
    if (!bulk_transfer_out(msd, shared_cbw, shared_cbw_phys, 31))
    {
        uart::printf("bot: CBW send failed\n");
        return -1;
    }

    // Data phase
    if (data_length > 0 && data_buf)
    {
        if (direction == USB_CBW_FLAG_IN)
        {
            sint32_t got = bulk_transfer_in(msd, data_buf, data_phys, data_length);
            if (got == -2)
                goto read_csw;
            if (got < 0)
                return -1;
        }
        else
        {
            if (!bulk_transfer_out(msd, data_buf, data_phys, data_length))
                return -1;
        }
    }

read_csw:
    // Status phase
    memory::memset((uint8_t*)shared_csw, 0, sizeof(usb_csw));
    sint32_t csw_got = bulk_transfer_in(msd, shared_csw, shared_csw_phys, 13);
    if (csw_got < 13)
    {
        uart::printf("bot: CSW receive failed\n");
        return -1;
    }

    if (shared_csw->dCSWSignature != USB_CSW_SIGNATURE)
    {
        uart::printf("bot: invalid CSW signature 0x%x\n", shared_csw->dCSWSignature);
        return -1;
    }

    if (shared_csw->bCSWStatus != 0)
    {
        uart::printf("bot: command 0x%x failed status=%u\n", (uint32_t)scsi_cmd[0],
                     (uint32_t)shared_csw->bCSWStatus);
        return (sint32_t)shared_csw->bCSWStatus;
    }

    return 0;
}

// SCSI commands

static bool scsi_inquiry(usb_mass_storage_dev* msd)
{
    uint8_t* data = (uint8_t*)alloc_xhci_memory(36, 64, 4096);
    uintptr_t data_phys = xhci_virt_to_phys(data);

    uint8_t cmd[6];
    memory::memset(cmd, 0, 6);
    cmd[0] = SCSI_INQUIRY;
    cmd[4] = 36;

    sint32_t result = bot_scsi_command(msd, cmd, 6, data, data_phys, 36, USB_CBW_FLAG_IN);
    if (result != 0)
    {
        uart::printf("scsi: INQUIRY failed\n");
        free_xhci_memory(data);
        return false;
    }

    // Save vendor/product strings into device registry
    for (uint8_t d = 0; d < device_count; d++)
    {
        if (device_infos[d].slot_id == msd->slot_id)
        {
            memory::memcpy((uint8_t*)device_infos[d].vendor_str, data + 8, 8);
            device_infos[d].vendor_str[8] = '\0';
            memory::memcpy((uint8_t*)device_infos[d].product_str, data + 16, 16);
            device_infos[d].product_str[16] = '\0';
            break;
        }
    }

    free_xhci_memory(data);
    return true;
}

// Wait for the device to accept commands. Real sticks need noticeably longer
// after reset than QEMU's emulated one, so the deadline is wall-clock (PIT),
// not a fixed retry count: poll TEST UNIT READY for up to 5 s.
static bool scsi_test_unit_ready(usb_mass_storage_dev* msd)
{
    uint8_t cmd[6];
    memory::memset(cmd, 0, 6);
    cmd[0] = SCSI_TEST_UNIT_READY;

    const uint32_t TIMEOUT_MS = 5000;
    const uint32_t POLL_MS    = 100;

    for (uint32_t waited = 0; waited <= TIMEOUT_MS; waited += POLL_MS)
    {
        sint32_t result = bot_scsi_command(msd, cmd, 6, nullptr, 0, 0, USB_CBW_FLAG_OUT);
        if (result == 0)
            return true;
        if (waited == TIMEOUT_MS)
            break;
        delay_ms(POLL_MS);
    }
    uart::printf("scsi: TEST UNIT READY timed out\n");
    return false;
}

static bool scsi_read_capacity(usb_mass_storage_dev* msd, uint32_t* out_last_lba, uint32_t* out_block_size)
{
    uint8_t* data = (uint8_t*)alloc_xhci_memory(8, 64, 4096);
    uintptr_t data_phys = xhci_virt_to_phys(data);

    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_READ_CAPACITY_10;

    sint32_t result = bot_scsi_command(msd, cmd, 10, data, data_phys, 8, USB_CBW_FLAG_IN);
    if (result != 0)
    {
        uart::printf("scsi: READ CAPACITY failed\n");
        free_xhci_memory(data);
        return false;
    }

    *out_last_lba =
        ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    *out_block_size =
        ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | (uint32_t)data[7];

    free_xhci_memory(data);
    return true;
}

static bool scsi_read_10(usb_mass_storage_dev* msd, uint32_t lba, uint16_t sector_count, void* buffer,
                         uintptr_t buffer_phys, uint32_t block_size)
{
    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_READ_10;
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)(lba);
    cmd[7] = (uint8_t)(sector_count >> 8);
    cmd[8] = (uint8_t)(sector_count);

    // The transfer length the CDB/CBW advertise must match the real sector
    // size: it used to be hardcoded *512, which silently transferred 1/8 of
    // the data on a 4096-byte-sector device.
    uint32_t byte_count = (uint32_t)sector_count * block_size;
    sint32_t result = bot_scsi_command(msd, cmd, 10, buffer, buffer_phys, byte_count, USB_CBW_FLAG_IN);
    return result == 0;
}

static bool scsi_write_10(usb_mass_storage_dev* msd, uint32_t lba, uint16_t sector_count, void* buffer,
                          uintptr_t buffer_phys, uint32_t block_size)
{
    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_WRITE_10;
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)(lba);
    cmd[7] = (uint8_t)(sector_count >> 8);
    cmd[8] = (uint8_t)(sector_count);

    uint32_t byte_count = (uint32_t)sector_count * block_size;
    sint32_t result = bot_scsi_command(msd, cmd, 10, buffer, buffer_phys, byte_count, USB_CBW_FLAG_OUT);
    return result == 0;
}

// Fetch the sense data of the command that just failed. Returns the sense
// key, or -1 when even that did not work.
static sint32_t scsi_request_sense(usb_mass_storage_dev* msd)
{
    const uint32_t len = 18;            // fixed-format sense data
    uint8_t* data = (uint8_t*)alloc_xhci_memory(len, 64, 4096);
    if (!data)
        return -1;
    memory::memset(data, 0, len);

    uint8_t cmd[6];
    memory::memset(cmd, 0, 6);
    cmd[0] = SCSI_REQUEST_SENSE;
    cmd[4] = (uint8_t)len;

    sint32_t result = bot_scsi_command(msd, cmd, 6, data, xhci_virt_to_phys(data),
                                       len, USB_CBW_FLAG_IN);
    sint32_t key = result == 0 ? (sint32_t)(data[2] & 0x0F) : -1;
    free_xhci_memory(data);
    return key;
}

static bool scsi_synchronize_cache(usb_mass_storage_dev* msd)
{
    if (msd->no_sync_cache)
        return true;

    uint8_t cmd[10];
    memory::memset(cmd, 0, 10);
    cmd[0] = SCSI_SYNCHRONIZE_CACHE;    // whole LBA range, no data phase
    sint32_t result = bot_scsi_command(msd, cmd, 10, nullptr, 0, 0, USB_CBW_FLAG_OUT);
    if (result == 0)
        return true;

    // Plenty of USB sticks do not implement SYNCHRONIZE CACHE and answer
    // with ILLEGAL REQUEST. They write through (or manage their cache on
    // their own), so there is nothing to flush. Anything else is a real
    // failure.
    if (result == 1 && scsi_request_sense(msd) == SCSI_SENSE_ILLEGAL_REQUEST)
    {
        uart::printf("scsi: SYNCHRONIZE CACHE not supported, flushes skipped\n");
        msd->no_sync_cache = true;
        return true;
    }
    return false;
}

// Allocate (once) the reusable per-device DMA bounce buffer.
static bool ensure_dma_buffer(uint8_t dev_index)
{
    if (msd_dma_buf[dev_index])
        return true;

    // One Normal TRB carries it, and a TRB's buffer may not cross a 64 KiB
    // boundary.
    uint32_t size = USB_MAX_XFER_BYTES;
    uint8_t* buf = (uint8_t*)alloc_xhci_memory(size, 64, 65536);
    if (!buf)
        return false;

    msd_dma_buf[dev_index]  = buf;
    msd_dma_phys[dev_index] = xhci_virt_to_phys(buf);
    msd_dma_size[dev_index] = size;
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

static bool get_device_descriptor(xhci_controller* hc, uint8_t slot_id, uint8_t port_speed, uint8_t port_index)
{
    usb_device_descriptor* desc = (usb_device_descriptor*)alloc_xhci_memory(sizeof(usb_device_descriptor), 64, 4096);
    uintptr_t desc_phys = xhci_virt_to_phys(desc);

    // Read first 8 bytes to get bMaxPacketSize0
    uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0};
    sint32_t got = control_transfer_in(hc, slot_id, setup, desc, desc_phys, 8);
    if (got < 8)
    {
        uart::printf("xhci: get device desc (8B) failed\n");
        free_xhci_memory(desc);
        return false;
    }

    // Update max packet size if needed
    uint16_t current_max_pkt = max_packet_size_for_speed(port_speed);
    uint16_t actual_max_pkt = desc->bMaxPacketSize0;
    if (port_speed >= 4 && actual_max_pkt <= 16)
        actual_max_pkt = (uint16_t)(1 << actual_max_pkt);

    if (actual_max_pkt != current_max_pkt)
    {
        if (!evaluate_context(hc, slot_id, actual_max_pkt))
        {
            free_xhci_memory(desc);
            return false;
        }
    }

    // Read full 18-byte descriptor
    memory::memset((uint8_t*)desc, 0, sizeof(usb_device_descriptor));
    setup[6] = 18;
    got = control_transfer_in(hc, slot_id, setup, desc, desc_phys, 18);
    if (got < 18)
    {
        uart::printf("xhci: get device desc (18B) failed\n");
        free_xhci_memory(desc);
        return false;
    }

    // Register in device info array (avoid duplicates)
    bool found = false;
    for (uint8_t d = 0; d < device_count; d++)
    {
        if (device_infos[d].slot_id == slot_id)
        {
            found = true;
            break;
        }
    }

    if (!found && device_count < MAX_USB_DEVICES)
    {
        usb_device_info* info = &device_infos[device_count++];
        info->slot_id = slot_id;
        info->port_index = port_index;
        info->port_speed = port_speed;
        info->vendor_id = desc->idVendor;
        info->product_id = desc->idProduct;
        info->bcd_usb = desc->bcdUSB;
        info->device_class = desc->bDeviceClass;
        info->device_subclass = desc->bDeviceSubClass;
        info->device_protocol = desc->bDeviceProtocol;
        info->is_mass_storage = false;
        info->connected = true;
        info->vendor_str[0] = '\0';
        info->product_str[0] = '\0';
    }

    free_xhci_memory(desc);
    return true;
}

static bool get_config_descriptor(xhci_controller* hc, uint8_t slot_id, uint8_t port_speed, uint8_t port_index)
{
    uint8_t* buf = (uint8_t*)alloc_xhci_memory(512, 64, 4096);
    uintptr_t buf_phys = xhci_virt_to_phys(buf);

    // Read 9-byte header first
    uint8_t setup[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 9, 0};
    sint32_t got = control_transfer_in(hc, slot_id, setup, buf, buf_phys, 9);
    if (got < 9)
    {
        uart::printf("xhci: get config desc header failed\n");
        free_xhci_memory(buf);
        return false;
    }

    usb_config_descriptor* cfg = (usb_config_descriptor*)buf;
    uint16_t total_len = cfg->wTotalLength;
    uint8_t config_value = cfg->bConfigurationValue;
    if (total_len > 512)
        total_len = 512;

    // Read full config descriptor
    memory::memset(buf, 0, 512);
    setup[6] = (uint8_t)(total_len & 0xFF);
    setup[7] = (uint8_t)(total_len >> 8);
    got = control_transfer_in(hc, slot_id, setup, buf, buf_phys, total_len);
    if (got < (sint32_t)total_len)
    {
        uart::printf("xhci: get config desc full failed\n");
        free_xhci_memory(buf);
        return false;
    }

    // Parse descriptor chain looking for mass storage interface
    uint16_t offset = 0;
    bool in_mass_storage = false;
    uint8_t bulk_in_ep = 0, bulk_out_ep = 0;
    uint16_t bulk_in_max_pkt = 0, bulk_out_max_pkt = 0;
    uint8_t iface_number = 0;

    while (offset + 2 <= total_len)
    {
        uint8_t desc_len = buf[offset];
        uint8_t desc_type = buf[offset + 1];
        if (desc_len == 0)
            break;

        if (desc_type == USB_DESC_TYPE_INTERFACE && desc_len >= 9)
        {
            usb_interface_descriptor* iface = (usb_interface_descriptor*)(buf + offset);

            if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE && iface->bInterfaceSubClass == USB_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_PROTOCOL_BBB)
            {
                in_mass_storage = true;
                iface_number = iface->bInterfaceNumber;

                // Mark in device registry
                for (uint8_t d = 0; d < device_count; d++)
                {
                    if (device_infos[d].slot_id == slot_id)
                    {
                        device_infos[d].is_mass_storage = true;
                        break;
                    }
                }
            }
            else
            {
                in_mass_storage = false;
            }
        }

        if (desc_type == USB_DESC_TYPE_ENDPOINT && desc_len >= 7 && in_mass_storage)
        {
            usb_endpoint_descriptor* ep = (usb_endpoint_descriptor*)(buf + offset);
            if ((ep->bmAttributes & 0x03) == 2)
            {
                if (ep->bEndpointAddress & 0x80)
                {
                    bulk_in_ep = ep->bEndpointAddress;
                    bulk_in_max_pkt = ep->wMaxPacketSize;
                }
                else
                {
                    bulk_out_ep = ep->bEndpointAddress;
                    bulk_out_max_pkt = ep->wMaxPacketSize;
                }
            }
        }
        offset += desc_len;
    }

    // Register mass storage device
    if (bulk_in_ep && bulk_out_ep && mass_storage_count < MAX_MASS_STORAGE_DEVS)
    {
        usb_mass_storage_dev* msd = &mass_storage_devs[mass_storage_count++];
        msd->hc = hc;
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
    }

    free_xhci_memory(buf);
    return true;
}

// Point the event routing at a mass storage device's bulk rings, or take
// them out of it.
static void msd_route_rings(usb_mass_storage_dev* msd, bool on)
{
    uint8_t in_dci = (msd->bulk_in_ep & 0x0F) * 2 + 1;
    uint8_t out_dci = (msd->bulk_out_ep & 0x0F) * 2;
    set_ring(msd->hc, msd->slot_id, in_dci, on ? &msd->bulk_in_ring : nullptr);
    set_ring(msd->hc, msd->slot_id, out_dci, on ? &msd->bulk_out_ring : nullptr);
}

static bool configure_mass_storage(usb_mass_storage_dev* msd)
{
    xhci_controller* hc = msd->hc;
    uint8_t slot_id = msd->slot_id;

    // SET_CONFIGURATION
    uint8_t setup[8] = {0x00, 0x09, msd->config_value, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (!control_transfer_no_data(hc, slot_id, setup))
    {
        uart::printf("xhci: SET_CONFIGURATION failed slot=%u\n", (uint32_t)slot_id);
        return false;
    }

    // Allocate transfer rings for bulk endpoints
    transfer_ring_init(&msd->bulk_in_ring, XHCI_TRANSFER_RING_TRB_COUNT);
    transfer_ring_init(&msd->bulk_out_ring, XHCI_TRANSFER_RING_TRB_COUNT);

    // Calculate DCI for each endpoint
    uint8_t in_ep_num = msd->bulk_in_ep & 0x0F;
    uint8_t out_ep_num = msd->bulk_out_ep & 0x0F;
    uint8_t in_dci = in_ep_num * 2 + 1;
    uint8_t out_dci = out_ep_num * 2;
    uint8_t max_dci = in_dci > out_dci ? in_dci : out_dci;
    msd_route_rings(msd, true);

    // Build Input Context for Configure Endpoint Command
    void* input_ctx = alloc_input_context(hc);
    if (!input_ctx) return false;

    input_control_ctx(input_ctx)->add_flags = (1 << 0) | (1 << in_dci) | (1 << out_dci);
    input_control_ctx(input_ctx)->drop_flags = 0;

    // Copy and update slot context
    void* out_ctx = (void*)hc->dcbaa_virt[slot_id];
    *input_slot_ctx(hc, input_ctx) = *device_slot_ctx(out_ctx);
    input_slot_ctx(hc, input_ctx)->context_entries = max_dci;

    // Bulk IN endpoint context
    xhci_endpoint_context* ep_in = input_ep_ctx(hc, input_ctx, in_dci);
    ep_in->endpoint_type = XHCI_EP_TYPE_BULK_IN;
    ep_in->max_packet_size = msd->bulk_in_max_packet;
    ep_in->max_burst_size = 0;
    ep_in->error_count = 3;
    ep_in->average_trb_length = 1024;
    ep_in->transfer_ring_dequeue_ptr = msd->bulk_in_ring.phys_base | 1;

    // Bulk OUT endpoint context
    xhci_endpoint_context* ep_out = input_ep_ctx(hc, input_ctx, out_dci);
    ep_out->endpoint_type = XHCI_EP_TYPE_BULK_OUT;
    ep_out->max_packet_size = msd->bulk_out_max_packet;
    ep_out->max_burst_size = 0;
    ep_out->error_count = 3;
    ep_out->average_trb_length = 1024;
    ep_out->transfer_ring_dequeue_ptr = msd->bulk_out_ring.phys_base | 1;

    // Send Configure Endpoint Command
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.control = (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 500);
    // Done with once the command completes; one that never did may still
    // be read by the controller, so it is left alone then.
    if (cc)
        free_xhci_memory(input_ctx);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: configure endpoint failed slot=%u code=%u\n", (uint32_t)slot_id,
                     cc ? (uint32_t)cc->completion_code : 0);
        return false;
    }

    msd->configured = true;
    return true;
}

static void setup_device(xhci_controller* hc, uint8_t port_index)
{
    xhci_portsc portsc = read_portsc(hc, port_index);
    uint8_t port_speed = portsc.port_speed;
    uint8_t port_id = port_index + 1;

    uint8_t slot_id = enable_device_slot(hc);
    if (slot_id == 0)
    {
        uart::printf("xhci: failed to enable slot for port %u\n", (uint32_t)port_index);
        return;
    }

    if (!create_device_context(hc, slot_id))
        return;

    // Allocate EP0 transfer ring
    xhci_transfer_ring* ep0_ring = &hc->ep0_rings[slot_id];
    transfer_ring_init(ep0_ring, XHCI_TRANSFER_RING_TRB_COUNT);
    set_ring(hc, slot_id, 1, ep0_ring);

    // Build Input Context for Address Device
    void* input_ctx = alloc_input_context(hc);
    if (!input_ctx)
        return;

    input_control_ctx(input_ctx)->add_flags = (1 << 0) | (1 << 1);
    input_control_ctx(input_ctx)->drop_flags = 0;

    xhci_slot_context* slot = input_slot_ctx(hc, input_ctx);
    slot->route_string = 0;
    slot->speed = port_speed;
    slot->context_entries = 1;
    slot->root_hub_port_num = port_id;

    xhci_endpoint_context* ep0 = input_ep_ctx(hc, input_ctx, 1);
    ep0->endpoint_type = XHCI_EP_TYPE_CONTROL_BIDIR;
    ep0->max_packet_size = max_packet_size_for_speed(port_speed);
    ep0->max_burst_size = 0;
    ep0->error_count = 3;
    ep0->average_trb_length = 8;
    ep0->transfer_ring_dequeue_ptr = ep0_ring->phys_base | 1;

    // Address Device Command
    xhci_trb_t cmd;
    memory::memset((uint8_t*)&cmd, 0, sizeof(xhci_trb_t));
    cmd.parameter = xhci_virt_to_phys(input_ctx);
    cmd.status = 0;
    cmd.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)slot_id << 24);

    xhci_cmd_completion_trb_t* cc = send_command(hc, &cmd, 500);
    // Done with once the command completes; one that never did may still
    // be read by the controller, so it is left alone then.
    if (cc)
        free_xhci_memory(input_ctx);
    if (!cc || cc->completion_code != XHCI_TRB_COMPLETION_SUCCESS)
    {
        uart::printf("xhci: address device failed port=%u\n", (uint32_t)port_index);
        return;
    }
    
    get_device_descriptor(hc, slot_id, port_speed, port_index);
    get_config_descriptor(hc, slot_id, port_speed, port_index);
}

// Capacity cache helper

static usb_status ensure_capacity_cached(uint8_t dev_index)
{
    if (dev_index >= mass_storage_count)
        return USB_ERR_NOT_FOUND;
    if (msd_capacity_cached[dev_index])
        return USB_OK;

    usb_mass_storage_dev* msd = &mass_storage_devs[dev_index];
    if (!msd->configured)
        return USB_ERR_NOT_READY;

    uint32_t last_lba, block_size;
    if (!scsi_read_capacity(msd, &last_lba, &block_size))
        return USB_ERR_IO;

    msd_last_lba[dev_index] = last_lba;
    msd_block_size[dev_index] = block_size;
    msd_capacity_cached[dev_index] = true;
    return USB_OK;
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

// Forget the devices found so far. A controller that is tried and dropped
// takes its devices with it.
static void reset_device_state()
{
    device_count = 0;
    mass_storage_count = 0;

    memory::memset((uint8_t*)mass_storage_devs, 0, sizeof(mass_storage_devs));
    memory::memset((uint8_t*)device_infos, 0, sizeof(device_infos));
    memory::memset((uint8_t*)msd_capacity_cached, 0, sizeof(msd_capacity_cached));
    memory::memset((uint8_t*)msd_block_size, 0, sizeof(msd_block_size));
    memory::memset((uint8_t*)msd_last_lba, 0, sizeof(msd_last_lba));

    // The bounce buffers are sized from the block size of the device that
    // used to hold this index, so they cannot be carried over to another
    // controller's devices.
    memory::memset((uint8_t*)msd_dma_buf, 0, sizeof(msd_dma_buf));
    memory::memset((uint8_t*)msd_dma_phys, 0, sizeof(msd_dma_phys));
    memory::memset((uint8_t*)msd_dma_size, 0, sizeof(msd_dma_size));
}

// Bring up one controller and enumerate what is attached to it. Leaves the
// controller running on success; `mass_storage_count` says what it found.
//
// Everything the controller had before is forgotten. The DMA allocations it
// made are not reclaimed - alloc_xhci_memory has no size-tracking free list
// and this runs a handful of times at boot - but nothing points at them any
// more.
static bool init_controller(xhci_controller* hc)
{
    PCIDevice* dev = hc->pci;
    memory::memset((uint8_t*)hc, 0, sizeof(*hc));
    hc->pci = dev;
    hc->ctx_entry_size = 32;
    reset_device_state();

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

    // Enumerate ports
    for (uint8_t i = 0; i < hc->max_ports; i++)
    {
        xhci_portsc portsc = read_portsc(hc, i);
        if (portsc.ccs)
        {
            if (reset_port(hc, i))
            {
                portsc = read_portsc(hc, i);
                process_events(hc);
                setup_device(hc, i);
            }
            else
            {
                uart::printf("xhci: port %u: reset failed\n", (uint32_t)i);
            }
        }
    }

    // Configure and prepare all mass storage devices. Slots that fail
    // configuration or never reach readiness are dropped: the block layer
    // above must not see a counted-but-unusable device.
    uint8_t ready_count = 0;
    for (uint8_t i = 0; i < mass_storage_count; i++)
    {
        usb_mass_storage_dev* msd = &mass_storage_devs[i];
        if (!configure_mass_storage(msd))
        {
            msd_route_rings(msd, false);
            continue;
        }

        scsi_inquiry(msd);
        if (!scsi_test_unit_ready(msd))
        {
            msd_route_rings(msd, false);
            continue;
        }

        if (ready_count != i)
        {
            // Its rings move with it: events must find them at the new place.
            mass_storage_devs[ready_count] = *msd;
            memory::memset((uint8_t*)msd, 0, sizeof(*msd));
            msd_route_rings(&mass_storage_devs[ready_count], true);
        }
        ready_count++;
    }
    mass_storage_count = ready_count;

    for (uint8_t i = 0; i < mass_storage_count; i++)
    {
        if (ensure_capacity_cached(i) != USB_OK)
            uart::printf("xhci: msd %u: READ CAPACITY failed\n", (uint32_t)i);
    }

    return true;
}

// Public API

namespace usb
{
    // Example using
    //  uint8_t sector[512];
    //  uint8_t data[512] = {0xDE, 0xAD, 0xBE, 0xEF}; // test data
    //  usb::read_sectors(0, 0, 1, sector);           // read MBR
    //  usb::write_sectors(0, 2, 1, data);            // write sector 2
    
    //  usb_block_device bdev;
    //  usb::get_block_device_info(0, &bdev);         // capacity info

    bool init()
    {
        shared_cbw = nullptr;
        shared_csw = nullptr;
        shared_cbw_phys = 0;
        shared_csw_phys = 0;
        bot_tag = 1;
        active = nullptr;
        controller_count = 0;
        reset_device_state();

        // Collect every xHCI controller. A desktop board usually has one (the
        // PCH at 00:14.0), but a mobile chipset commonly adds a second one for
        // its USB4/Thunderbolt ports - and that one sits at a *lower* device
        // number (00:0d.0 on Alder Lake-P), so "the first xHCI on the bus" is
        // the controller with no user-facing ports on exactly the machines
        // where that matters.
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

        if (controller_count == 0)
        {
            uart::printf("xhci: no controller found\n");
            return false;
        }

        uart::printf("xhci: %u controller(s) found\n", (uint32_t)controller_count);

        // Try them in turn and keep the one that actually has our storage on
        // it. A controller that yields no mass storage is halted again before
        // the next attempt, so an abandoned one cannot keep DMAing into
        // memory its successor is about to allocate.
        uint8_t best = 0;
        uint8_t best_devices = 0;
        bool have_best = false;

        for (uint8_t i = 0; i < controller_count; i++)
        {
            xhci_controller* hc = &controllers[i];
            if (init_controller(hc) && mass_storage_count > 0)
            {
                active = hc;
                uart::printf("xhci: using controller %u:%u.%u (%u disk(s))\n",
                             (uint32_t)hc->pci->bus,
                             (uint32_t)hc->pci->device,
                             (uint32_t)hc->pci->function,
                             (uint32_t)mass_storage_count);
                return true;
            }

            if (!have_best || device_count > best_devices)
            {
                best = i;
                best_devices = device_count;
                have_best = true;
            }

            uart::printf("xhci: controller %u:%u.%u: %u device(s), no storage\n",
                         (uint32_t)hc->pci->bus,
                         (uint32_t)hc->pci->device,
                         (uint32_t)hc->pci->function,
                         (uint32_t)device_count);

            if (i + 1 < controller_count)
                stop_controller(hc);
        }

        // No storage anywhere. Leave the controller that at least saw devices
        // running, so lsusb/usbinfo still have something to report. The last
        // one tried is still running; it stops before the best one restarts.
        if (best_devices > 0 && best != controller_count - 1)
        {
            stop_controller(&controllers[controller_count - 1]);
            init_controller(&controllers[best]);
            active = &controllers[best];
        }
        else
            active = &controllers[controller_count - 1];

        return false;
    }

    const char* get_usb_class_name(uint8_t cls)
    {
        return usb_class_name(cls);
    }

    const char* get_usb_speed_str(uint8_t speed)
    {
        return usb_speed_str(speed);
    }

    uint32_t get_context_entry_size()
    {
        return active ? active->ctx_entry_size : 32;
    }

    uint8_t get_port_count()
    {
        return active && active->op_regs ? active->max_ports : 0;
    }

    // Raw PORTSC of one root port, for `usbports`. Zero when no controller
    // came up, which the caller reports as such.
    uint32_t get_port_status(uint8_t port)
    {
        if (!active || !active->op_regs || port >= active->max_ports)
            return 0;
        return read_portsc(active, port).raw;
    }

    bool port_is_usb3(uint8_t port)
    {
        return active && is_usb3_port(active, port);
    }

    uint8_t get_controller_count()
    {
        return controller_count;
    }

    // Bus/device/function of the controller we are driving, or 0:0.0 when
    // none came up.
    void get_controller_location(uint8_t* bus, uint8_t* dev, uint8_t* fn)
    {
        *bus = active ? active->pci->bus : 0;
        *dev = active ? active->pci->device : 0;
        *fn  = active ? active->pci->function : 0;
    }

    uint8_t get_device_count()
    {
        return device_count;
    }

    usb_status get_device_info(uint8_t index, usb_device_info* out)
    {
        if (index >= device_count)
            return USB_ERR_NOT_FOUND;
        if (!out)
            return USB_ERR_INVALID_PARAM;
        *out = device_infos[index];
        return USB_OK;
    }

    uint8_t get_block_device_count()
    {
        return mass_storage_count;
    }

    usb_status get_block_device_info(uint8_t index, usb_block_device* out)
    {
        if (index >= mass_storage_count)
            return USB_ERR_NOT_FOUND;
        if (!out)
            return USB_ERR_INVALID_PARAM;

        usb_status st = ensure_capacity_cached(index);
        if (st != USB_OK)
            return st;

        out->device_index = index;
        out->block_size = msd_block_size[index];
        out->last_lba = msd_last_lba[index];
        out->total_bytes = ((uint64_t)msd_last_lba[index] + 1) * msd_block_size[index];
        out->ready = mass_storage_devs[index].configured;
        return USB_OK;
    }

    usb_status read_sectors(uint8_t dev_index, uint32_t lba, uint16_t count, void* buffer)
    {
        if (dev_index >= mass_storage_count)
            return USB_ERR_NOT_FOUND;
        if (!buffer || count == 0)
            return USB_ERR_INVALID_PARAM;

        usb_mass_storage_dev* msd = &mass_storage_devs[dev_index];
        if (!msd->configured)
            return USB_ERR_NOT_READY;

        usb_status st = ensure_capacity_cached(dev_index);
        if (st != USB_OK)
            return st;

        // One request may not exceed the reusable DMA buffer; the block layer
        // above splits larger transfers.
        if ((uint32_t)count * msd_block_size[dev_index] > USB_MAX_XFER_BYTES)
            return USB_ERR_INVALID_PARAM;

        // Bounds-check against the capacity READ CAPACITY reported: a bogus
        // LBA from a broken filesystem must never reach the device.
        if ((uint64_t)lba + count > (uint64_t)msd_last_lba[dev_index] + 1)
            return USB_ERR_INVALID_PARAM;

        if (!ensure_dma_buffer(dev_index))
            return USB_ERR_IO;

        uint32_t bs = msd_block_size[dev_index];
        uint32_t total = (uint32_t)count * bs;

        uint8_t* dma_buf = msd_dma_buf[dev_index];
        uintptr_t dma_phys = msd_dma_phys[dev_index];

        bool ok = scsi_read_10(msd, lba, count, dma_buf, dma_phys, bs);
        if (ok)
            memory::memcpy((uint8_t*)buffer, dma_buf, total);

        return ok ? USB_OK : USB_ERR_IO;
    }

    usb_status write_sectors(uint8_t dev_index, uint32_t lba, uint16_t count, const void* buffer)
    {
        if (dev_index >= mass_storage_count)
            return USB_ERR_NOT_FOUND;
        if (!buffer || count == 0)
            return USB_ERR_INVALID_PARAM;

        usb_mass_storage_dev* msd = &mass_storage_devs[dev_index];
        if (!msd->configured)
            return USB_ERR_NOT_READY;

        usb_status st = ensure_capacity_cached(dev_index);
        if (st != USB_OK)
            return st;

        if ((uint32_t)count * msd_block_size[dev_index] > USB_MAX_XFER_BYTES)
            return USB_ERR_INVALID_PARAM;

        if ((uint64_t)lba + count > (uint64_t)msd_last_lba[dev_index] + 1)
            return USB_ERR_INVALID_PARAM;

        if (!ensure_dma_buffer(dev_index))
            return USB_ERR_IO;

        uint32_t bs = msd_block_size[dev_index];
        uint32_t total = (uint32_t)count * bs;

        uint8_t* dma_buf = msd_dma_buf[dev_index];
        uintptr_t dma_phys = msd_dma_phys[dev_index];

        memory::memcpy(dma_buf, (uint8_t*)buffer, total);
        bool ok = scsi_write_10(msd, lba, count, dma_buf, dma_phys, bs);

        return ok ? USB_OK : USB_ERR_IO;
    }

    usb_status flush_cache(uint8_t dev_index)
    {
        if (dev_index >= mass_storage_count)
            return USB_ERR_NOT_FOUND;

        usb_mass_storage_dev* msd = &mass_storage_devs[dev_index];
        if (!msd->configured)
            return USB_ERR_NOT_READY;

        return scsi_synchronize_cache(msd) ? USB_OK : USB_ERR_IO;
    }

} // namespace usb