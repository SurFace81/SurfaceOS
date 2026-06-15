#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "ports.h"

#define PCI_MAX_DEVICES 128

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

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

struct PCIDevice 
{
    uint8_t  bus, device, function;
    uint16_t vendor_id, device_id, command, status;
    uint8_t  revision_id, prog_if, subclass, class_code;
    uint8_t  cache_line_size, latency_timer, header_type, bist;
    uint32_t bar[6], cardbus_cis;
    uint16_t subsystem_vendor_id, subsystem_id;
    uint32_t expansion_rom;
    uint8_t  capabilities, interrupt_line, interrupt_pin;
    bool     valid;
};

struct PCIBar 
{
    bool     valid;
    bool     is_io;
    uint64_t base;
};

namespace pci 
{
    void init(void);

    uint32_t   device_count();
    PCIDevice* get_by_id(uint32_t idx);
    PCIDevice* find(uint8_t cls, uint8_t subcls, int prog_if = -1, uint32_t start_idx = 0);

    void enable_device(PCIDevice* d);
    PCIBar get_bar(PCIDevice* d, int index = 0);

    uint32_t read32 (PCIDevice* d, uint8_t off);
    uint16_t read16 (PCIDevice* d, uint8_t off);
    void     write32(PCIDevice* d, uint8_t off, uint32_t val);
    void     write16(PCIDevice* d, uint8_t off, uint16_t val);
    
    const char* class_name(uint8_t class_code);
}

#endif