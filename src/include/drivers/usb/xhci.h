#ifndef XHCI_H
#define XHCI_H

#include "../../cpu/types.h"
#include "../../cpu/pci.h"
#include "../../cpu/paging.h"
#include "../../mm/memory.h"
#include "../../mm/heap.h"
#include "../uart.h"

// xHCI register structures (spec section 5.3)

struct xhci_cap_regs {
    const uint8_t  caplength;
    const uint8_t  reserved0;
    const uint16_t hciversion;
    const uint32_t hcsparams1;
    const uint32_t hcsparams2;
    const uint32_t hcsparams3;
    const uint32_t hccparams1;
    const uint32_t dboff;
    const uint32_t rtsoff;
    const uint32_t hccparams2;
} __attribute__((packed));

// Port Status and Control Register (spec section 5.4.8)
struct xhci_portsc {
    union {
        struct {
            uint32_t ccs        : 1;
            uint32_t ped        : 1;
            uint32_t rsvd0      : 1;
            uint32_t oca        : 1;
            uint32_t pr         : 1;
            uint32_t pls        : 4;
            uint32_t pp         : 1;
            uint32_t port_speed : 4;
            uint32_t pic        : 2;
            uint32_t lws        : 1;
            uint32_t csc        : 1;
            uint32_t pec        : 1;
            uint32_t wrc        : 1;
            uint32_t occ        : 1;
            uint32_t prc        : 1;
            uint32_t plc        : 1;
            uint32_t cec        : 1;
            uint32_t cas        : 1;
            uint32_t wce        : 1;
            uint32_t wde        : 1;
            uint32_t woe        : 1;
            uint32_t rsvd1      : 2;
            uint32_t dr         : 1;
            uint32_t wpr        : 1;
        } __attribute__((packed));
        uint32_t raw;
    };
} __attribute__((packed));

// Operational registers (spec section 5.4)
struct xhci_op_regs {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint32_t reserved0[2];
    uint32_t dnctrl;
    uint64_t crcr;
    uint32_t reserved1[4];
    uint64_t dcbaap;
    uint32_t config;
    uint32_t reserved2[49];
} __attribute__((packed));

// Interrupter registers (spec section 5.5.2)
struct xhci_interrupter_regs {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t rsvd;
    uint64_t erstba;
    union {
        struct {
            uint64_t dequeue_erst_segment_index : 3;
            uint64_t event_handler_busy         : 1;
            uint64_t event_ring_dequeue_pointer : 60;
        };
        uint64_t erdp;
    };
} __attribute__((packed));

// Runtime registers (spec section 5.5)
struct xhci_runtime_regs {
    uint32_t mf_index;
    uint32_t rsvdz[7];
    xhci_interrupter_regs ir[1024];
} __attribute__((packed));

// Event Ring Segment Table entry (spec section 6.5)
struct xhci_erst_entry {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size;
    uint32_t rsvd;
} __attribute__((packed));

// TRB structure (spec section 4.11)
struct xhci_trb_t {
    uint64_t parameter;
    uint32_t status;
    union {
        struct {
            uint32_t cycle_bit               : 1;
            uint32_t eval_next_trb           : 1;
            uint32_t interrupt_on_short_pkt  : 1;
            uint32_t no_snoop                : 1;
            uint32_t chain_bit               : 1;
            uint32_t interrupt_on_completion : 1;
            uint32_t immediate_data          : 1;
            uint32_t rsvd0                   : 2;
            uint32_t block_event_interrupt   : 1;
            uint32_t trb_type                : 6;
            uint32_t rsvd1                   : 16;
        };
        uint32_t control;
    };
} __attribute__((packed));

// Command Completion Event TRB (spec section 6.4.2.2)
struct xhci_cmd_completion_trb_t {
    uint64_t command_trb_pointer;
    struct {
        uint32_t rsvd0           : 24;
        uint32_t completion_code : 8;
    };
    struct {
        uint32_t cycle_bit   : 1;
        uint32_t rsvd1       : 9;
        uint32_t trb_type    : 6;
        uint32_t vfid        : 8;
        uint32_t slot_id     : 8;
    };
} __attribute__((packed));

// Doorbell register (spec section 5.6)
struct xhci_doorbell_reg {
    union {
        struct {
            uint8_t  db_target;
            uint8_t  rsvd;
            uint16_t db_stream_id;
        };
        uint32_t raw;
    };
} __attribute__((packed));

// Slot Context (spec section 6.2.2)
struct xhci_slot_context {
    union {
        struct {
            uint32_t route_string   : 20;
            uint32_t speed          : 4;
            uint32_t rz             : 1;
            uint32_t mtt            : 1;
            uint32_t hub            : 1;
            uint32_t context_entries: 5;
        };
        uint32_t dword0;
    };
    union {
        struct {
            uint16_t max_exit_latency;
            uint8_t  root_hub_port_num;
            uint8_t  port_count;
        };
        uint32_t dword1;
    };
    union {
        struct {
            uint32_t parent_hub_slot_id : 8;
            uint32_t parent_port_number : 8;
            uint32_t tt_think_time      : 2;
            uint32_t rsvd0              : 4;
            uint32_t interrupter_target : 10;
        };
        uint32_t dword2;
    };
    union {
        struct {
            uint32_t device_address : 8;
            uint32_t rsvd1          : 19;
            uint32_t slot_state     : 5;
        };
        uint32_t dword3;
    };
    uint32_t rsvdz[4];
} __attribute__((packed));

// Endpoint Context (spec section 6.2.3)
struct xhci_endpoint_context {
    union {
        struct {
            uint32_t endpoint_state      : 3;
            uint32_t rsvd0               : 5;
            uint32_t mult                : 2;
            uint32_t max_primary_streams : 5;
            uint32_t linear_stream_array : 1;
            uint32_t interval            : 8;
            uint32_t max_esit_payload_hi : 8;
        };
        uint32_t dword0;
    };
    union {
        struct {
            uint32_t rsvd1               : 1;
            uint32_t error_count         : 2;
            uint32_t endpoint_type       : 3;
            uint32_t rsvd2               : 1;
            uint32_t host_initiate_disable : 1;
            uint32_t max_burst_size      : 8;
            uint32_t max_packet_size     : 16;
        };
        uint32_t dword1;
    };
    union {
        struct {
            uint64_t dcs                          : 1;
            uint64_t rsvd3                        : 3;
            uint64_t tr_dequeue_ptr_address_bits  : 60;
        };
        uint64_t transfer_ring_dequeue_ptr;
    };
    union {
        struct {
            uint16_t average_trb_length;
            uint16_t max_esit_payload_lo;
        };
        uint32_t dword4;
    };
    uint32_t padding[3];
} __attribute__((packed));

// Device Context (spec section 6.2.1)
struct xhci_device_context {
    xhci_slot_context      slot_context;
    xhci_endpoint_context  control_ep_context;
    xhci_endpoint_context  ep[30];
} __attribute__((packed));

// Input Control Context (spec section 6.2.5.1)
struct xhci_input_control_context {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t rsvd[5];
    uint8_t  config_value;
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  rsvdZ;
} __attribute__((packed));

// Input Context (spec section 6.2.5)
struct xhci_input_context {
    xhci_input_control_context control_context;
    xhci_device_context        device_context;
} __attribute__((packed));

// Transfer Event completion TRB
struct xhci_transfer_event_trb_t {
    uint64_t trb_pointer;
    struct {
        uint32_t transfer_length : 24;
        uint32_t completion_code : 8;
    };
    struct {
        uint32_t cycle_bit    : 1;
        uint32_t rsvd0        : 1;
        uint32_t event_data   : 1;
        uint32_t rsvd1        : 7;
        uint32_t trb_type     : 6;
        uint32_t endpoint_id  : 5;
        uint32_t rsvd2        : 3;
        uint32_t slot_id      : 8;
    };
} __attribute__((packed));

// USBCMD bits
#define XHCI_USBCMD_RUN_STOP           (1 << 0)
#define XHCI_USBCMD_HCRESET            (1 << 1)
#define XHCI_USBCMD_INTERRUPTER_ENABLE (1 << 2)
#define XHCI_USBCMD_HOSTSYS_ERR_EN     (1 << 3)
#define XHCI_USBCMD_LIGHT_HCRESET      (1 << 7)

// USBSTS bits
#define XHCI_USBSTS_HCH    (1 << 0)
#define XHCI_USBSTS_HSE    (1 << 2)
#define XHCI_USBSTS_EINT   (1 << 3)
#define XHCI_USBSTS_PCD    (1 << 4)
#define XHCI_USBSTS_SSS    (1 << 8)
#define XHCI_USBSTS_RSS    (1 << 9)
#define XHCI_USBSTS_SRE    (1 << 10)
#define XHCI_USBSTS_CNR    (1 << 11)
#define XHCI_USBSTS_HCE    (1 << 12)

// IMAN / ERDP
#define XHCI_IMAN_INTERRUPT_PENDING  (1 << 0)
#define XHCI_IMAN_INTERRUPT_ENABLE   (1 << 1)
#define XHCI_ERDP_EHB               (1 << 3)

// HCSPARAMS / HCCPARAMS macros
#define XHCI_NEXT_EXT_CAP_PTR(ptr, next) (volatile uint32_t*)((char*)(ptr) + ((next) * sizeof(uint32_t)))
#define XHCI_PPC(regs)                   (((regs)->hccparams1 >> 3) & 0x1)
#define XHCI_PIND(regs)                  (((regs)->hccparams1 >> 4) & 0x1)
#define XHCI_LHRC(regs)                  (((regs)->hccparams1 >> 5) & 0x1)
#define XHCI_IST(regs)                   ((regs)->hcsparams2 & 0xF)
#define XHCI_ERST_MAX(regs)              (((regs)->hcsparams2 >> 4) & 0xF)
#define XHCI_MAX_DEVICE_SLOTS(r)         ((r)->hcsparams1 & 0xFF)
#define XHCI_MAX_INTERRUPTERS(r)         (((r)->hcsparams1 >> 8) & 0x7FF)
#define XHCI_MAX_PORTS(r)               (((r)->hcsparams1 >> 24) & 0xFF)
#define XHCI_MAX_SCRATCHPAD_BUFS_HI(r)  (((r)->hcsparams2 >> 21) & 0x1F)
#define XHCI_MAX_SCRATCHPAD_BUFS_LO(r)  (((r)->hcsparams2 >> 27) & 0x1F)
#define XHCI_MAX_SCRATCHPAD_BUFFERS(r)   ((XHCI_MAX_SCRATCHPAD_BUFS_HI(r) << 5) | XHCI_MAX_SCRATCHPAD_BUFS_LO(r))
#define XHCI_AC64(r)                     ((r)->hccparams1 & 0x1)
#define XHCI_CSZ(r)                      (((r)->hccparams1 >> 2) & 0x1)
#define XHCI_XECP(r)                     (((r)->hccparams1 >> 16) & 0xFFFF)

// Legacy / extended caps
#define XHCI_LEGACY_SUPPORT_CAP_ID  1
#define XHCI_LEGACY_BIOS_OWNED      (1 << 16)
#define XHCI_LEGACY_OS_OWNED        (1 << 24)
#define XHCI_EXT_CAP_PROTOCOL       2

// Memory alignment requirements
#define XHCI_DCBAA_ALIGNMENT            64
#define XHCI_DCBAA_BOUNDARY             4096
#define XHCI_SCRATCHPAD_BUF_ALIGNMENT   4096
#define XHCI_SCRATCHPAD_BUF_BOUNDARY    4096
#define XHCI_CMD_RING_ALIGNMENT         64
#define XHCI_CMD_RING_BOUNDARY          65536
#define XHCI_EVT_RING_ALIGNMENT         64
#define XHCI_EVT_RING_BOUNDARY          65536
#define XHCI_ERST_ALIGNMENT             64
#define XHCI_ERST_BOUNDARY              4096
#define XHCI_DEVICE_CTX_ALIGNMENT       64
#define XHCI_DEVICE_CTX_BOUNDARY        4096
#define XHCI_INPUT_CTX_ALIGNMENT        64
#define XHCI_INPUT_CTX_BOUNDARY         4096
#define XHCI_TRANSFER_RING_ALIGNMENT    64
#define XHCI_TRANSFER_RING_BOUNDARY     65536

// Ring sizes
#define XHCI_COMMAND_RING_TRB_COUNT     256
#define XHCI_EVENT_RING_TRB_COUNT       256
#define XHCI_TRANSFER_RING_TRB_COUNT    256

// TRB types - commands
#define XHCI_TRB_TYPE_LINK                      6
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD           9
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD    10
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD        11
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD    12
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD      13
#define XHCI_TRB_TYPE_RESET_ENDPOINT_CMD        14
#define XHCI_TRB_TYPE_NOOP_CMD                  23

// TRB types - transfers
#define XHCI_TRB_TYPE_NORMAL          1
#define XHCI_TRB_TYPE_SETUP_STAGE     2
#define XHCI_TRB_TYPE_DATA_STAGE      3
#define XHCI_TRB_TYPE_STATUS_STAGE    4

// TRB types - events
#define XHCI_TRB_TYPE_TRANSFER_EVENT            32
#define XHCI_TRB_TYPE_CMD_COMPLETION_EVENT      33
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT  34

// TRB field helpers
#define XHCI_TRB_TYPE_SHIFT             10
#define XHCI_CRCR_RING_CYCLE_STATE      (1 << 0)
#define XHCI_LINK_TRB_TC_BIT            (1 << 1)
#define XHCI_TRB_COMPLETION_SUCCESS     1
#define XHCI_TRB_COMPLETION_STALL       6
#define XHCI_TRB_COMPLETION_SHORT_PACKET 13

// Device Context Indexes: 1 is the control endpoint, 2..31 the others.
#define XHCI_MAX_DCI 32

// Endpoint types (Endpoint Context EP Type)
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTERRUPT_OUT  3
#define XHCI_EP_TYPE_CONTROL_BIDIR  4
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTERRUPT_IN   7

// Doorbell targets
#define XHCI_DOORBELL_TARGET_COMMAND_RING   0
#define XHCI_DOORBELL_TARGET_CONTROL_EP     1


// Max USB3 ports we track
#define XHCI_MAX_USB3_PORTS 32

// A mobile chipset can expose a PCH xHCI and a separate USB4/Thunderbolt one.
#define MAX_XHCI_CONTROLLERS 8

// Device slots we enable per controller (CONFIG.MaxSlotsEn). Controllers in
// the wild offer 32-64; slot IDs index per-slot tables, so they are capped.
#define XHCI_MAX_SLOTS 64

// Port bring-up timing. 20 ms is the spec's settle time after setting PP;
// the scan window covers USB3 link training plus a USB2 device's debounce.
#define XHCI_PORT_POWER_SETTLE_MS   20
#define XHCI_PORT_SCAN_TIMEOUT_MS   1000
#define XHCI_PORT_POLL_INTERVAL_MS  10
#define XHCI_PORT_DEBOUNCE_MS       100


// ---------------------------------------------------------------------------
// The controller as the USB core (usb.cpp) sees it. Everything here works on
// one controller and is synchronous: a call returns when the controller has
// answered or the timeout has passed.

// A transfer ring: the TRBs the controller reads for one endpoint.
struct xhci_transfer_ring
{
    xhci_trb_t* trbs;
    uintptr_t phys_base;
    size_t max_trb_count;
    size_t enqueue_ptr;
    uint8_t cycle_bit;

    // How the transfer last started on this ring ended, filled in by the
    // event processing: `done` once its final event arrived, with that
    // event's completion code, and the bytes a short packet left untouched.
    volatile bool done;
    uint8_t cc;
    uint32_t residue;
};

// One endpoint to configure on a device (Configure Endpoint command).
struct xhci_ep_config
{
    uint8_t dci;                // Device Context Index: ep_num * 2 + (IN ? 1 : 0)
    uint8_t type;               // XHCI_EP_TYPE_*
    uint16_t max_packet;
    uint8_t interval;           // Endpoint Context Interval (2^n * 125 us)
    xhci_transfer_ring* ring;   // initialised by configure_endpoints()
};

struct xhci_controller;

namespace xhci
{
    // Find every xHCI controller on the PCI bus; how many there are.
    uint8_t find_controllers();
    xhci_controller* controller(uint8_t index);
    PCIDevice* pci_device(xhci_controller* hc);

    // Reset and start a controller, power its ports and wait for what is
    // attached to show up. False when it cannot be brought up.
    bool start(xhci_controller* hc);
    // Halt it: it stops reading our rings.
    void stop(xhci_controller* hc);

    // Root ports, 0-based.
    uint8_t port_count(xhci_controller* hc);
    uint32_t port_status(xhci_controller* hc, uint8_t port);     // raw PORTSC
    bool port_is_usb3(xhci_controller* hc, uint8_t port);
    bool port_connected(xhci_controller* hc, uint8_t port);
    // Reset a connected port; true when it came out enabled.
    bool reset_port(xhci_controller* hc, uint8_t port);
    uint8_t port_speed(xhci_controller* hc, uint8_t port);        // XHCI speed ID
    uint32_t context_entry_size(xhci_controller* hc);

    // Device slots. enable_slot() returns the slot ID, 0 on failure.
    uint8_t enable_slot(xhci_controller* hc);
    // Address the device on `port` in `slot`, with `ep0` as its control
    // ring (initialised here) and `max_packet` as EP0's packet size.
    bool address_device(xhci_controller* hc, uint8_t slot, uint8_t port, uint8_t speed,
                        uint16_t max_packet, xhci_transfer_ring* ep0);
    // EP0's real packet size, once the device descriptor told it.
    bool set_ep0_max_packet(xhci_controller* hc, uint8_t slot, uint16_t max_packet);
    // Add endpoints to a device, rings and all, in one command.
    bool configure_endpoints(xhci_controller* hc, uint8_t slot, xhci_ep_config* eps, uint8_t count);
    // After a halt: reset the endpoint and move its dequeue pointer past
    // whatever was left on the ring.
    bool reset_endpoint(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring);

    // Transfers. False on timeout; otherwise ring->cc and ring->residue
    // say how it went.
    //
    // A control transfer: `setup` is the 8-byte setup packet; `in` says the
    // direction of a data stage of `length` bytes at `data_phys` (none when
    // length is 0).
    bool control(xhci_controller* hc, uint8_t slot, xhci_transfer_ring* ep0, const uint8_t* setup,
                 uintptr_t data_phys, uint16_t length, bool in, uint32_t timeout_ms);
    // One Normal TRB of `length` bytes on endpoint `dci`.
    bool normal(xhci_controller* hc, uint8_t slot, uint8_t dci, xhci_transfer_ring* ring,
                uintptr_t data_phys, uint32_t length, uint32_t timeout_ms);
    bool completed_ok(const xhci_transfer_ring* ring);     // SUCCESS or SHORT_PACKET
    const char* completion_code_str(uint8_t code);

    // Memory the controller reads or writes: `size` bytes aligned to
    // `alignment` that do not cross a `boundary` (0: any), zeroed.
    void* dma_alloc(size_t size, size_t alignment, size_t boundary);
    void dma_free(void* ptr);
    uintptr_t phys(void* vaddr);

    // Busy wait on the PIT; usable before anything is scheduled.
    void delay_ms(uint32_t ms);
}

#endif // XHCI_H
