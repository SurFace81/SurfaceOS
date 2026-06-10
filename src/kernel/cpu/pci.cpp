#include "../../include/cpu/pci.h"

static PCIDevice devices[PCI_MAX_DEVICES];
static uint32_t count = 0;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
}

static uint32_t cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    port::dword_out(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, fn, off));
    return port::dword_in(PCI_CONFIG_DATA);
}

static uint16_t cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t val = cfg_read32(bus, dev, fn, off);
    return (val >> ((off & 2) * 8)) & 0xFFFF;
}

static uint8_t cfg_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t val = cfg_read32(bus, dev, fn, off);
    return (val >> ((off & 3) * 8)) & 0xFF;
}

static void cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val)
{
    port::dword_out(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, fn, off));
    port::dword_out(PCI_CONFIG_DATA, val);
}

static void cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val)
{
    port::dword_out(PCI_CONFIG_ADDRESS, cfg_addr(bus, dev, fn, off));
    port::word_out(PCI_CONFIG_DATA + (off & 2), val);
}

static bool cfg_exists(uint8_t bus, uint8_t dev, uint8_t fn)
{
    return cfg_read16(bus, dev, fn, PCI_VENDOR_ID) != 0xFFFF;
}

static void read_device(PCIDevice& d, uint8_t bus, uint8_t dev, uint8_t fn)
{
    d = {};
    d.bus = bus;
    d.device = dev;
    d.function = fn;

    d.vendor_id = cfg_read16(bus, dev, fn, PCI_VENDOR_ID);
    d.device_id = cfg_read16(bus, dev, fn, PCI_DEVICE_ID);
    d.command = cfg_read16(bus, dev, fn, PCI_COMMAND);
    d.status = cfg_read16(bus, dev, fn, PCI_STATUS);
    d.revision_id = cfg_read8(bus, dev, fn, PCI_REVISION_ID);
    d.prog_if = cfg_read8(bus, dev, fn, PCI_PROG_IF);
    d.subclass = cfg_read8(bus, dev, fn, PCI_SUBCLASS);
    d.class_code = cfg_read8(bus, dev, fn, PCI_CLASS);
    d.cache_line_size = cfg_read8(bus, dev, fn, PCI_CACHE_LINE_SIZE);
    d.latency_timer = cfg_read8(bus, dev, fn, PCI_LATENCY_TIMER);
    d.header_type = cfg_read8(bus, dev, fn, PCI_HEADER_TYPE);
    d.bist = cfg_read8(bus, dev, fn, PCI_BIST);

    for (int i = 0; i < 6; i++)
        d.bar[i] = cfg_read32(bus, dev, fn, PCI_BAR0 + i * 4);

    d.cardbus_cis = cfg_read32(bus, dev, fn, PCI_CARDBUS_CIS);
    d.subsystem_vendor_id = cfg_read16(bus, dev, fn, PCI_SUBSYSTEM_VENDOR_ID);
    d.subsystem_id = cfg_read16(bus, dev, fn, PCI_SUBSYSTEM_ID);
    d.expansion_rom = cfg_read32(bus, dev, fn, PCI_EXPANSION_ROM);
    d.capabilities = cfg_read8(bus, dev, fn, PCI_CAPABILITIES);
    d.interrupt_line = cfg_read8(bus, dev, fn, PCI_INTERRUPT_LINE);
    d.interrupt_pin = cfg_read8(bus, dev, fn, PCI_INTERRUPT_PIN);

    d.valid = true;
}

static void scan_bus(uint8_t bus);
static void scan_dev(uint8_t bus, uint8_t dev);
static void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn);

static void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (!cfg_exists(bus, dev, fn) || count >= PCI_MAX_DEVICES)
        return;

    read_device(devices[count++], bus, dev, fn);

    PCIDevice& last = devices[count - 1];
    if (last.class_code == PCI_CLASS_BRIDGE && last.subclass == 0x04)
        scan_bus(cfg_read8(bus, dev, fn, 0x19));
}

static void scan_dev(uint8_t bus, uint8_t dev)
{
    if (!cfg_exists(bus, dev, 0))
        return;

    scan_fn(bus, dev, 0);

    if (cfg_read8(bus, dev, 0, PCI_HEADER_TYPE) & 0x80)
    {
        for (uint8_t fn = 1; fn < 8; fn++)
            scan_fn(bus, dev, fn);
    }
}

static void scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; dev++)
        scan_dev(bus, dev);
}

static void scan_all()
{
    count = 0;
    for (uint32_t i = 0; i < PCI_MAX_DEVICES; i++)
        devices[i].valid = false;

    if (!(cfg_read8(0, 0, 0, PCI_HEADER_TYPE) & 0x80))
    {
        scan_bus(0);
    }
    else
    {
        for (uint8_t fn = 0; fn < 8; fn++)
        {
            if (cfg_exists(0, 0, fn))
                scan_bus(fn);
        }
    }
}

namespace pci
{
    void init()
    {
        scan_all();
    }

    uint32_t device_count()
    {
        return count;
    }

    PCIDevice* get_by_id(uint32_t idx)
    {
        return (idx < count) ? &devices[idx] : nullptr;
    }

    PCIDevice* find(uint8_t cls, uint8_t subcls, int prog_if, uint32_t start_idx)
    {
        for (uint32_t i = start_idx; i < count; i++)
        {
            PCIDevice* d = &devices[i];
            if (!d->valid)
                continue;
            if (d->class_code != cls || d->subclass != subcls)
                continue;
            if (prog_if >= 0 && d->prog_if != (uint8_t)prog_if)
                continue;
            return d;
        }
        return nullptr;
    }

    void enable_device(PCIDevice* d)
    {
        uint16_t cmd = read16(d, PCI_COMMAND);
        cmd |= (1 << 0) | (1 << 1) | (1 << 2);
        write16(d, PCI_COMMAND, cmd);
        d->command = cmd;
    }

    PCIBar get_bar(PCIDevice* d, int index)
    {
        PCIBar bar = {};
        if (!d || index < 0 || index > 5)
        {
            return bar;
        }

        uint32_t raw = d->bar[index];
        if (!raw)
        {
            return bar;
        }

        if (raw & 1)
        {
            bar.is_io = true;
            bar.base = raw & ~0x3u;
        }
        else
        {
            uint8_t type = (raw >> 1) & 0x3;
            if (type == 0x00)
            {
                bar.base = raw & 0xFFFFFFF0u;
            }
            else if (type == 0x02)
            {
                if (index == 5)
                    return bar;
                bar.base = ((uint64_t)d->bar[index + 1] << 32) | (raw & 0xFFFFFFF0u);
            }
            else
            {
                return bar;
            }
        }

        bar.valid = (bar.base != 0) || bar.is_io;
        return bar;
    }

    uint32_t read32(PCIDevice* d, uint8_t off)
    {
        return cfg_read32(d->bus, d->device, d->function, off);
    }

    uint16_t read16(PCIDevice* d, uint8_t off)
    {
        return cfg_read16(d->bus, d->device, d->function, off);
    }

    void write32(PCIDevice* d, uint8_t off, uint32_t val)
    {
        cfg_write32(d->bus, d->device, d->function, off, val);
    }

    void write16(PCIDevice* d, uint8_t off, uint16_t val)
    {
        cfg_write16(d->bus, d->device, d->function, off, val);
    }

} // namespace pci