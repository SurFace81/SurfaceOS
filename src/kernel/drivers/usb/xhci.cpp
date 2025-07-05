#include "../../../include/drivers/usb/xhci.h"

namespace xhci {
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

    struct XHCIPortRegs {
        UINT32 portsc;
        UINT32 portpmsc;
        UINT32 portli;
        UINT32 porthlpmc;
    } __attribute__((packed));

    static PCIDevice* controller_pci = nullptr;
    static XHCICapabilityRegs* cap_regs = nullptr;
    static XHCIOperationalRegs* op_regs = nullptr;
    static XHCIPortRegs* port_regs = nullptr;
    static UINT8 max_ports = 0;
    static USBDeviceList device_list = {0};

    static PCIDevice* find_xhci_controller() {
        for (UINT32 i = 0; i < pci::device_count(); i++) {
            PCIDevice* device = pci::get_by_id(i);
            if (device && device->valid && 
                device->class_code == 0x0C && 
                device->subclass == 0x03 && 
                device->prog_if == 0x30) {
                return device;
            }
        }
        return nullptr;
    }

    static bool map_registers() {
        if (!controller_pci) return false;

        UINT64 base_address = controller_pci->bar[0] & ~0xF;
        if (base_address == 0) return false;

        cap_regs = (XHCICapabilityRegs*)base_address;
        if (cap_regs->caplength == 0 || cap_regs->caplength > 0xFF) return false;

        op_regs = (XHCIOperationalRegs*)(base_address + cap_regs->caplength);
        port_regs = (XHCIPortRegs*)(base_address + cap_regs->caplength + 0x400);

        max_ports = (cap_regs->hcsparams1 >> 24) & 0xFF;
        if (max_ports > XHCI_MAX_PORTS) max_ports = XHCI_MAX_PORTS;

        return true;
    }

    static void reset_controller() {
        // Stop controller
        op_regs->usbcmd &= ~0x1;
        for (int i = 0; i < 1000000 && !(op_regs->usbsts & 0x1); i++);

        // Reset controller
        op_regs->usbcmd |= 0x2;
        for (int i = 0; i < 1000000 && (op_regs->usbcmd & 0x2); i++);
        for (int i = 0; i < 1000000 && (op_regs->usbsts & 0x800); i++);
    }

    static void start_controller() {
        op_regs->usbcmd |= 0x1;
        for (int i = 0; i < 1000000 && (op_regs->usbsts & 0x1); i++);
    }

    static UINT32 get_port_speed(UINT32 portsc) {
        if (!(portsc & 0x1)) return 0;
        return (portsc >> 10) & 0xF;
    }

    static void reset_ports() {
        for (UINT8 i = 0; i < max_ports; i++) {
            UINT32* portsc = &port_regs[i].portsc;
            if ((*portsc & 0x1) && !(*portsc & (1 << 4))) {
                *portsc |= (1 << 4); // PR
                for (int j = 0; j < 100000 && (*portsc & (1 << 21)); j++);
            }
        }
    }

    static void scan_ports() {
        device_list.length = 0;
        if (!port_regs || max_ports == 0) return;

        for (UINT8 i = 0; i < max_ports; i++) {
            UINT32 portsc = port_regs[i].portsc;

            USBDevice* device = &device_list.devices[device_list.length];
            device->port_number = i + 1;
            device->speed = get_port_speed(portsc);
            device->connected = (portsc & 0x1) ? true : false;
            device->vendor_id = 0;
            device->device_id = 0;

            if (device->connected) device_list.length++;
        }
    }

    void init() {
        controller_pci = find_xhci_controller();
        if (!controller_pci) return;

        if (!map_registers()) return;

        reset_controller();

        // Базовая конфигурация
        op_regs->config = (cap_regs->hcsparams1 & 0x1F);  // Max slots

        // DCBAAP указывает на массив дескрипторов устройств (можно инициализировать позже)
        op_regs->dcbaap = 0;

        // Командное кольцо (упрощено: crcr = 0 пока)
        op_regs->crcr = 0;

        start_controller();
        reset_ports();
        scan_ports();
    }

    UINT32 device_count() {
        scan_ports();
        return device_list.length;
    }

    USBDevice* get_device(UINT32 idx) {
        if (idx >= device_list.length) return nullptr;
        return &device_list.devices[idx];
    }

    USBDeviceList* get_device_list() {
        scan_ports();
        return &device_list;
    }

    const char* get_speed_name(UINT32 speed) {
        switch (speed) {
            case 0: return "Disconnected";
            case 1: return "Low Speed";
            case 2: return "Full Speed";
            case 3: return "High Speed";
            case 4: return "Super Speed";
            default: return "Unknown";
        }
    }
}