#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "ports.h"

#define PCI_MAX_DEVICES 128

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_MAX_DEVICES     128

// PCI Configuration Space offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_CARDBUS_CIS     0x28
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID    0x2E
#define PCI_EXPANSION_ROM   0x30
#define PCI_CAPABILITIES    0x34
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI Device Classes
#define PCI_CLASS_UNCLASSIFIED      0x00
#define PCI_CLASS_STORAGE           0x01
#define PCI_CLASS_NETWORK           0x02
#define PCI_CLASS_DISPLAY           0x03
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_CLASS_MEMORY            0x05
#define PCI_CLASS_BRIDGE            0x06
#define PCI_CLASS_COMMUNICATION     0x07
#define PCI_CLASS_SYSTEM            0x08
#define PCI_CLASS_INPUT             0x09
#define PCI_CLASS_DOCKING           0x0A
#define PCI_CLASS_PROCESSOR         0x0B
#define PCI_CLASS_SERIAL            0x0C

struct PCIDevice {
    UINT8  bus, device, function;
    UINT16 vendor_id, device_id, command, status;
    UINT8  revision_id, prog_if, subclass, class_code;
    UINT8  cache_line_size, latency_timer, header_type, bist;
    UINT32 bar[6], cardbus_cis;
    UINT16 subsystem_vendor_id, subsystem_id;
    UINT32 expansion_rom;
    UINT8  capabilities, interrupt_line, interrupt_pin;
    bool   valid;
};

struct ClassDevices {
    int length;
    PCIDevice cls_devices[PCI_MAX_DEVICES];
};

namespace pci {
    void init(void);
    UINT32 device_count();
    PCIDevice* get_by_id(UINT32 idx);
    //ClassDevices* pci::get_by_class(UINT8 cls);
}

#endif