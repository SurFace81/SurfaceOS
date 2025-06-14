#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "ports.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_CLASS_DISPLAY  0x03
#define PCI_SUBCLASS_VGA   0x00

void find_vga_device();

#ifdef __cplusplus
}
#endif

#endif  // PCI_H