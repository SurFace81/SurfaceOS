#include "../../include/cpu/pci.h"

UINT32 pci_config_read(UINT8 bus, UINT8 slot, UINT8 function, UINT8 offset) {
    UINT32 address = (1 << 31) | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFC);
    asm volatile ("outl %0, %%dx" :: "a"(address), "d"(PCI_CONFIG_ADDRESS));
    UINT32 result;
    asm volatile ("inl %%dx, %0" : "=a"(result) : "d"(PCI_CONFIG_DATA));
    return result;
}

void pci_config_write(UINT8 bus, UINT8 slot, UINT8 function, UINT8 offset, UINT32 value) {
    UINT32 address = (1 << 31) | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFC);
    asm volatile ("outl %0, %%dx" :: "a"(address), "d"(PCI_CONFIG_ADDRESS));
    asm volatile ("outl %0, %%dx" :: "a"(value), "d"(PCI_CONFIG_DATA));
}

void find_vga_device() {
    for (UINT8 bus = 0; bus < 256; bus++) {
        for (UINT8 slot = 0; slot < 32; slot++) {
            UINT16 vendor_id = pci_config_read(bus, slot, 0, 0) & 0xFFFF;
            if (vendor_id == 0xFFFF) {
                continue; // No device present
            }

            UINT32 class_code = pci_config_read(bus, slot, 0, 8);
            UINT8 class = (class_code >> 24) & 0xFF;
            UINT8 subclass = (class_code >> 16) & 0xFF;
        }
    }
}
