#ifndef XHCI_H
#define XHCI_H

#include "../../cpu/types.h"
#include "../../cpu/pci.h"
#include "../../cpu/paging.h"
#include "../../mm/memory.h"
#include "../../mm/heap.h"
#include "../uart.h"

// Capability registers (xHCI spec section 5.3)
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

// Operational registers (xHCI spec section 5.4)
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

// Interrupter registers (xHCI spec section 5.5.2)
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

// Runtime registers (xHCI spec section 5.5)
struct xhci_runtime_regs {
    uint32_t mf_index;
    uint32_t rsvdz[7];
    xhci_interrupter_regs ir[1024];
} __attribute__((packed));

// Event Ring Segment Table entry (xHCI spec section 6.5)
struct xhci_erst_entry {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size;
    uint32_t rsvd;
} __attribute__((packed));

// TRB structure (xHCI spec section 4.11)
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

// Command Completion Event TRB (xHCI spec section 6.4.2.2)
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

// Doorbell register (xHCI spec section 5.6)
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

// IMAN bits
#define XHCI_IMAN_INTERRUPT_PENDING  (1 << 0)
#define XHCI_IMAN_INTERRUPT_ENABLE   (1 << 1)

// ERDP bits
#define XHCI_ERDP_EHB               (1 << 3)

// HCSPARAMS1
#define XHCI_MAX_DEVICE_SLOTS(regs)  ((regs)->hcsparams1 & 0xFF)
#define XHCI_MAX_INTERRUPTERS(regs)  (((regs)->hcsparams1 >> 8) & 0x7FF)
#define XHCI_MAX_PORTS(regs)         (((regs)->hcsparams1 >> 24) & 0xFF)

// HCSPARAMS2
#define XHCI_IST(regs)                     ((regs)->hcsparams2 & 0xF)
#define XHCI_ERST_MAX(regs)                (((regs)->hcsparams2 >> 4) & 0xF)
#define XHCI_MAX_SCRATCHPAD_BUFS_HI(regs)  (((regs)->hcsparams2 >> 21) & 0x1F)
#define XHCI_MAX_SCRATCHPAD_BUFS_LO(regs)  (((regs)->hcsparams2 >> 27) & 0x1F)
#define XHCI_MAX_SCRATCHPAD_BUFFERS(regs)  \
    ((XHCI_MAX_SCRATCHPAD_BUFS_HI(regs) << 5) | XHCI_MAX_SCRATCHPAD_BUFS_LO(regs))

// HCCPARAMS1
#define XHCI_AC64(regs)  ((regs)->hccparams1 & 0x1)
#define XHCI_BNC(regs)   (((regs)->hccparams1 >> 1) & 0x1)
#define XHCI_CSZ(regs)   (((regs)->hccparams1 >> 2) & 0x1)
#define XHCI_PPC(regs)   (((regs)->hccparams1 >> 3) & 0x1)
#define XHCI_PIND(regs)  (((regs)->hccparams1 >> 4) & 0x1)
#define XHCI_LHRC(regs)  (((regs)->hccparams1 >> 5) & 0x1)
#define XHCI_XECP(regs)  (((regs)->hccparams1 >> 16) & 0xFFFF)

// Legacy support
#define XHCI_LEGACY_SUPPORT_CAP_ID       1
#define XHCI_LEGACY_BIOS_OWNED           (1 << 16)
#define XHCI_LEGACY_OS_OWNED             (1 << 24)
#define XHCI_NEXT_EXT_CAP_PTR(ptr, next) \
    (volatile uint32_t*)((char*)(ptr) + ((next) * sizeof(uint32_t)))

// Memory alignment
#define XHCI_DCBAA_ALIGNMENT            64
#define XHCI_DCBAA_BOUNDARY             4096
#define XHCI_SCRATCHPAD_BUF_ALIGNMENT   4096
#define XHCI_SCRATCHPAD_BUF_BOUNDARY    4096

// Command ring
#define XHCI_COMMAND_RING_TRB_COUNT     256
#define XHCI_CMD_RING_ALIGNMENT         64
#define XHCI_CMD_RING_BOUNDARY          65536

// Event ring
#define XHCI_EVENT_RING_TRB_COUNT       256
#define XHCI_EVT_RING_ALIGNMENT         64
#define XHCI_EVT_RING_BOUNDARY          65536
#define XHCI_ERST_ALIGNMENT             64
#define XHCI_ERST_BOUNDARY              4096

// TRB types
#define XHCI_TRB_TYPE_RESERVED                  0
#define XHCI_TRB_TYPE_NORMAL                    1
#define XHCI_TRB_TYPE_SETUP_STAGE               2
#define XHCI_TRB_TYPE_DATA_STAGE                3
#define XHCI_TRB_TYPE_STATUS_STAGE              4
#define XHCI_TRB_TYPE_ISOCH                     5
#define XHCI_TRB_TYPE_LINK                      6
#define XHCI_TRB_TYPE_EVENT_DATA                7
#define XHCI_TRB_TYPE_NOOP                      8
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD           9
#define XHCI_TRB_TYPE_DISABLE_SLOT_CMD          10
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD        11
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD    12
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD      13
#define XHCI_TRB_TYPE_RESET_ENDPOINT_CMD        14
#define XHCI_TRB_TYPE_STOP_ENDPOINT_CMD         15
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD    16
#define XHCI_TRB_TYPE_RESET_DEVICE_CMD          17
#define XHCI_TRB_TYPE_FORCE_EVENT_CMD           18
#define XHCI_TRB_TYPE_NOOP_CMD                  23
#define XHCI_TRB_TYPE_TRANSFER_EVENT            32
#define XHCI_TRB_TYPE_CMD_COMPLETION_EVENT      33
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT  34
#define XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT     37

// TRB field helpers
#define XHCI_TRB_TYPE_SHIFT             10
#define XHCI_TRB_TYPE_MASK              0xFC00
#define XHCI_CRCR_RING_CYCLE_STATE      (1 << 0)
#define XHCI_LINK_TRB_TC_BIT            (1 << 1)

// TRB completion codes
#define XHCI_TRB_COMPLETION_INVALID                 0
#define XHCI_TRB_COMPLETION_SUCCESS                 1
#define XHCI_TRB_COMPLETION_DATA_BUFFER_ERROR       2
#define XHCI_TRB_COMPLETION_BABBLE_DETECTED         3
#define XHCI_TRB_COMPLETION_USB_TRANSACTION_ERROR   4
#define XHCI_TRB_COMPLETION_TRB_ERROR               5
#define XHCI_TRB_COMPLETION_STALL_ERROR             6
#define XHCI_TRB_COMPLETION_RESOURCE_ERROR          7
#define XHCI_TRB_COMPLETION_BANDWIDTH_ERROR         8
#define XHCI_TRB_COMPLETION_NO_SLOTS_AVAILABLE      9
#define XHCI_TRB_COMPLETION_SHORT_PACKET            13
#define XHCI_TRB_COMPLETION_EVENT_RING_FULL         21
#define XHCI_TRB_COMPLETION_COMMAND_RING_STOPPED    24
#define XHCI_TRB_COMPLETION_COMMAND_ABORTED         25

// Doorbell targets
#define XHCI_DOORBELL_TARGET_COMMAND_RING   0
#define XHCI_DOORBELL_TARGET_CONTROL_EP     1

// Construct a command TRB
#define XHCI_CONSTRUCT_CMD_TRB(type) \
    xhci_trb_t { 0, 0, { .control = (type) << XHCI_TRB_TYPE_SHIFT } }

namespace xhci {
    bool init(void);
}

#endif // XHCI_H