#include "../../include/cpu/pci.h"

static PCIDevice devices[PCI_MAX_DEVICES];
static uint32_t count = 0;
static ClassDevices cls_devs;

namespace pci {
    void scan_dev(uint8_t bus, uint8_t dev);
    void scan_bus(uint8_t bus);
    void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn);
    void scan_all(void);
    
    static uint32_t addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
        return (1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
    }

    static uint32_t read_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
        port::dword_out(PCI_CONFIG_ADDRESS, addr(bus, dev, fn, off));
        return port::dword_in(PCI_CONFIG_DATA);
    }

    static uint16_t read_word(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
        uint32_t val = read_dword(bus, dev, fn, off & 0xFC);
        return (val >> ((off & 2) * 8)) & 0xFFFF;
    }

    static uint8_t read_byte(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
        uint32_t val = read_dword(bus, dev, fn, off & 0xFC);
        return (val >> ((off & 3) * 8)) & 0xFF;
    }

    static bool exists(uint8_t bus, uint8_t dev, uint8_t fn) {
        return read_word(bus, dev, fn, 0x00) != 0xFFFF;
    }

    static void read(PCIDevice& device, uint8_t bus, uint8_t dev, uint8_t fn) {
        device = {};
        device.bus = bus; device.device = dev; device.function = fn;
        device.vendor_id        = read_word(bus, dev, fn, 0x00);
        device.device_id        = read_word(bus, dev, fn, 0x02);
        device.command          = read_word(bus, dev, fn, 0x04);
        device.status           = read_word(bus, dev, fn, 0x06);
        device.revision_id      = read_byte(bus, dev, fn, 0x08);
        device.prog_if          = read_byte(bus, dev, fn, 0x09);
        device.subclass         = read_byte(bus, dev, fn, 0x0A);
        device.class_code       = read_byte(bus, dev, fn, 0x0B);
        device.cache_line_size  = read_byte(bus, dev, fn, 0x0C);
        device.latency_timer    = read_byte(bus, dev, fn, 0x0D);
        device.header_type      = read_byte(bus, dev, fn, 0x0E);
        device.bist             = read_byte(bus, dev, fn, 0x0F);
        for (int i = 0; i < 6; i++) {
            device.bar[i] = read_dword(bus, dev, fn, 0x10 + i * 4);
        }            
        device.cardbus_cis      = read_dword(bus, dev, fn, 0x28);
        device.subsystem_vendor_id = read_word(bus, dev, fn, 0x2C);
        device.subsystem_id     = read_word(bus, dev, fn, 0x2E);
        device.expansion_rom    = read_dword(bus, dev, fn, 0x30);
        device.capabilities     = read_byte(bus, dev, fn, 0x34);
        device.interrupt_line   = read_byte(bus, dev, fn, 0x3C);
        device.interrupt_pin    = read_byte(bus, dev, fn, 0x3D);
        device.valid = true;
    }

    void scan_bus(uint8_t bus) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            scan_dev(bus, dev);
        }
    }

    void scan_dev(uint8_t bus, uint8_t dev) {
        if (!exists(bus, dev, 0)) return;

        scan_fn(bus, dev, 0);
        if (read_byte(bus, dev, 0, 0x0E) & 0x80) {
            for (uint8_t fn = 1; fn < 8; fn++) {
                scan_fn(bus, dev, fn);
            }
        }                
    }

    void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
        if (!exists(bus, dev, fn) || count >= PCI_MAX_DEVICES) return;

        read(devices[count++], bus, dev, fn);
        if (devices[count - 1].class_code == 0x06 && devices[count - 1].subclass == 0x04) {
            scan_bus(read_byte(bus, dev, fn, 0x19));
        }            
    }

    void scan_all(void) {
        count = 0;
        for (uint32_t i = 0; i < PCI_MAX_DEVICES; i++) {
            devices[i].valid = false;
        }

        if ((read_byte(0, 0, 0, 0x0E) & 0x80) == 0) {
            scan_bus(0);
        } else {
            for (uint8_t fn = 0; fn < 8; fn++) {
                if (exists(0, 0, fn)) {
                    scan_bus(fn);
                }
            }
        }
    }

    void init(void) {
        scan_all();
    }

    uint32_t device_count() { return count; }

    PCIDevice* get_by_id(uint32_t idx) {
        if (idx >= count) {
            return nullptr;
        }

        return &devices[idx];
    }

    // ClassDevices* get_by_class(uint8_t cls) {
    //     cls_devs.length = 0;
    //     for (int i = 0; i < PCI_MAX_DEVICES; i++) {
    //         if (devices[i].class_code == cls) {
    //             cls_devs.cls_devices[cls_devs.length] = devices[i];
    //             cls_devs.length += 1;
    //         }
    //     }

    //     return &cls_devs;
    // }
} // namespace