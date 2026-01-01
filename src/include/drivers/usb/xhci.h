#ifndef XHCI_H
#define XHCI_H

#include "../../cpu/types.h"
#include "../../cpu/pci.h"

#define XHCI_MAX_PORTS 32

struct USBDevice {
    UINT8  port_number;
    UINT32 speed;       // 0 = disconnected, 1 = low, 2 = full, 3 = high, 4 = super
    bool   connected;
    UINT16 vendor_id;
    UINT16 device_id;
};
struct USBDeviceList {
    int length;
    USBDevice devices[XHCI_MAX_PORTS];
};

 struct XHCIOperationalRegs {
     UINT32 usbcmd;
     UINT32 usbsts;
     UINT32 pagesize;
     UINT32 reserved1[2];
     UINT32 dnctrl;
     UINT64 crcr;
     UINT32 reserved2[4];
     UINT64 dcbaap;
     UINT32 config;
     UINT32 reserved3[49];
 } __attribute__((packed));

 struct XHCICapabilityRegs {
    UINT8  caplength;
    UINT8  reserved;
    UINT16 hciversion;
    UINT32 hcsparams1;
    UINT32 hcsparams2;
    UINT32 hcsparams3;
    UINT32 hccparams1;
    UINT32 dboff;
    UINT32 rtsoff;
    UINT32 hccparams2;
} __attribute__((packed));

struct XHCITrb {
    UINT64 parameter;
    UINT32 status;
    union {
        struct {
            UINT32 cycle_bit               : 1;
            UINT32 eval_next_trb           : 1;
            UINT32 interrupt_on_short_pkt  : 1;
            UINT32 no_snoop                : 1;
            UINT32 chain_bit               : 1;
            UINT32 interrupt_on_completion : 1;
            UINT32 immediate_data          : 1;
            UINT32 rsvd0                   : 2;
            UINT32 block_event_interrupt   : 1;
            UINT32 trb_type                : 6;
            UINT32 rsvd1                   : 16;
        };
        UINT32 control;
    };
} __attribute__((packed));

struct XHCIInterrupterRegs {
    UINT32 iman;         // Interrupter Management
    UINT32 imod;         // Interrupter Moderation
    UINT32 erstsz;       // Event Ring Segment Table Size
    UINT32 rsvd;         // Reserved
    UINT64 erstba;       // Event Ring Segment Table Base Address
    union {
        struct {
            // This index is used to accelerate the checking of
            // an Event Ring Full condition. This field can be 0.
            UINT64 dequeue_erst_segment_index : 3;

            // This bit is set by the controller when it sets the
            // Interrupt Pending bit. Then once your handler is finished
            // handling the event ring, you clear it by writing a '1' to it.
            UINT64 event_handler_busy         : 1;

            // Physical address of the _next_ item in the event ring
            UINT64 event_ring_dequeue_pointer : 60;
        };
        UINT64 erdp;     // Event Ring Dequeue Pointer (offset 18h)
    };
};

struct XHCIRuntimeRegs {
    UINT32 mf_index;                      // Microframe Index (offset 0000h)
    UINT32 rsvdz[7];                      // Reserved (offset 001Fh:0004h)
    XHCIInterrupterRegs ir[1024];  // Interrupter Register Sets (offset 0020h to 8000h)
};

struct XHCIErstEntry {
    UINT64 ring_segment_base_addr;
    UINT32 ring_segment_size;
    UINT32 rsvd;
} __attribute__((packed));

struct XHCIPortRegs {
    UINT32 portsc;
    UINT32 portpmsc;
    UINT32 portli;
    UINT32 porthlpmc;
} __attribute__((packed));

namespace xhci {
    void init(void);
    UINT32 device_count();
    USBDevice* get_device(UINT32 idx);
    USBDeviceList* get_device_list();
    const char* get_speed_name(UINT32 speed);
}

#endif