#ifndef XHCI_H
#define XHCI_H

#include "../../cpu/types.h"
#include "../../cpu/pci.h"

#define XHCI_MAX_PORTS 32

struct USBDevice {
    uint8_t  port_number;
    uint32_t speed;       // 0 = disconnected, 1 = low, 2 = full, 3 = high, 4 = super
    bool   connected;
    uint16_t vendor_id;
    uint16_t device_id;
};
struct USBDeviceList {
    int length;
    USBDevice devices[XHCI_MAX_PORTS];
};

 struct XHCIOperationalRegs {
     uint32_t usbcmd;
     uint32_t usbsts;
     uint32_t pagesize;
     uint32_t reserved1[2];
     uint32_t dnctrl;
     uint64_t crcr;
     uint32_t reserved2[4];
     uint64_t dcbaap;
     uint32_t config;
     uint32_t reserved3[49];
 } __attribute__((packed));

 struct XHCICapabilityRegs {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
} __attribute__((packed));

struct XHCITrb {
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

struct XHCIInterrupterRegs {
    uint32_t iman;         // Interrupter Management
    uint32_t imod;         // Interrupter Moderation
    uint32_t erstsz;       // Event Ring Segment Table Size
    uint32_t rsvd;         // Reserved
    uint64_t erstba;       // Event Ring Segment Table Base Address
    union {
        struct {
            // This index is used to accelerate the checking of
            // an Event Ring Full condition. This field can be 0.
            uint64_t dequeue_erst_segment_index : 3;

            // This bit is set by the controller when it sets the
            // Interrupt Pending bit. Then once your handler is finished
            // handling the event ring, you clear it by writing a '1' to it.
            uint64_t event_handler_busy         : 1;

            // Physical address of the _next_ item in the event ring
            uint64_t event_ring_dequeue_pointer : 60;
        };
        uint64_t erdp;     // Event Ring Dequeue Pointer (offset 18h)
    };
};

struct XHCIRuntimeRegs {
    uint32_t mf_index;                      // Microframe Index (offset 0000h)
    uint32_t rsvdz[7];                      // Reserved (offset 001Fh:0004h)
    XHCIInterrupterRegs ir[1024];  // Interrupter Register Sets (offset 0020h to 8000h)
};

struct XHCIErstEntry {
    uint64_t ring_segment_base_addr;
    uint32_t ring_segment_size;
    uint32_t rsvd;
} __attribute__((packed));

struct XHCIPortRegs {
    uint32_t portsc;
    uint32_t portpmsc;
    uint32_t portli;
    uint32_t porthlpmc;
} __attribute__((packed));

namespace xhci {
    void init(void);
    uint32_t device_count();
    USBDevice* get_device(uint32_t idx);
    USBDeviceList* get_device_list();
    const char* get_speed_name(uint32_t speed);
}

#endif