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
 } __attribute__((packed));

namespace xhci {
    void init(void);
    UINT32 device_count();
    USBDevice* get_device(UINT32 idx);
    USBDeviceList* get_device_list();
    const char* get_speed_name(UINT32 speed);
    bool init_usb_flash_drive();
    bool read_usb_sector(UINT32 lba, void* buffer);
}

#endif