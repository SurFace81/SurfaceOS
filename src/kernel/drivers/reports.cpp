// What the kernel has to say about the machine: the text reports behind
// SfAdmin Report (sfadmin.cpp) - the processor, PCI and USB devices, disks,
// mounts, ACPI, memory, the kernel's log. See reports.h.

#include "../../include/drivers/reports.h"
#include "../../include/drivers/screen.h"
#include "../../include/cpu/cpuid.h"
#include "../../include/cpu/pci.h"
#include "../../include/dev/blkdev.h"
#include "../../include/fs/vfs.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/usb/usb.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/pmm.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/smp.h"
#include "../../include/acpi/acpi.h"

static void cmd_cpuid(int argc, const char** argv)
{
    char name[64];
    cpuid::get_cpu_name(name);

    CPUTopology topo;
    cpuid::get_cpu_topology(&topo);

    CacheInfo cache;
    cpuid::get_cache_info(&cache);

    screen::printf("\n\r");
    screen::printf("\n\r CPU:            %s", name);
    if (cpuid::get_base_freq())
    {
        screen::printf("\n\r Base freq:      %u MHz", cpuid::get_base_freq());
        screen::printf("\n\r Max freq:       %u MHz", cpuid::get_max_freq());
        screen::printf("\n\r Bus freq:       %u MHz", cpuid::get_bus_freq());
    }
    else
        screen::printf("\n\r Frequencies:    not reported by the CPU");
    screen::printf("\n\r Logical CPUs:   %u, %u started", topo.logical_cores, smp::running());
    screen::printf("\n\r Physical cores: %u%s", topo.physical_cores,
                   topo.hybrid ? " (performance + efficiency)" : "");
    screen::printf("\n\r Sockets:        %u", topo.packages);
    screen::printf("\n\r Hyperthreading: %s", topo.hyperthreading ? "Yes" : "No");
    const char* whose = topo.hybrid ? "of the core this runs on" : "per core";
    if (!cache.l1d_size && !cache.l2_size)
        screen::printf("\n\r Caches:         not reported by the CPU");
    else
    {
        screen::printf("\n\r L1 cache:       %u KB data + %u KB code, %s",
                       cache.l1d_size, cache.l1i_size, whose);
        screen::printf("\n\r L2 cache:       %u KB, %s", cache.l2_size, whose);
        if (cache.l3_size)
            screen::printf("\n\r L3 cache:       %u KB, shared", cache.l3_size);
    }
}

static void cmd_lspci(int argc, const char** argv)
{
    uint32_t count = pci::device_count();
    screen::printf("\n\r");
    screen::printf("\n\r PCI devices found: %u", count);
    screen::printf("\n\r");

    for (uint32_t i = 0; i < count; i++)
    {
        PCIDevice* d = pci::get_by_id(i);

        char vid[5], did[5];
        hex_to_str(d->vendor_id, vid, 4);
        hex_to_str(d->device_id, did, 4);

        screen::printf("\n\r  %u:%u.%u  0x%s:0x%s  %s",
            (uint32_t)d->bus, (uint32_t)d->device, (uint32_t)d->function,
            vid, did,
            pci::class_name(d->class_code));
    }
}

// Where everything is mounted.
static void cmd_mount(int argc, const char** argv)
{
    (void)argc; (void)argv;
    screen::printf("\n\r");
    for (uint32_t i = 0; vfs::mount_count_get(i); i++)
    {
        mount* m = vfs::mount_count_get(i);
        if (m->point)
        {
            char buf[PATH_MAX];
            if (vfs::get_path(m->point, buf, sizeof(buf), nullptr) == 0)
                screen::printf("  %s on %s\n\r", m->devname, buf);
        }
        else
            screen::printf("  %s on /\n\r", m->devname);
    }
}

// dmesg: the kernel boot log. Everything the drivers report goes to the
// serial line, which no laptop has, so keep a copy on screen too.
static void cmd_dmesg(int argc, const char** argv)
{
    static char buf[UART_LOG_SIZE + 1];
    uint32_t len = uart::log_read(buf, UART_LOG_SIZE);
    buf[len] = '\0';

    screen::printf("\n\r");
    for (uint32_t i = 0; i < len; i++)
    {
        // The log uses bare newlines; the console wants CR with them.
        if (buf[i] == '\n')
            screen::printf("\n\r");
        else
            screen::printf("%c", buf[i]);
    }
}

// usbports: the raw root-port state of every controller. The one thing
// worth photographing when a machine enumerates nothing: it separates "no
// controller", "port unpowered", "nothing plugged in" and "device present but
// enumeration failed".
static void cmd_usbports(int argc, const char** argv)
{
    uint8_t controllers = usb::get_controller_count();
    screen::printf("\n\r");
    if (controllers == 0)
    {
        screen::printf("\n\r No controller found");
        return;
    }

    for (uint8_t c = 0; c < controllers; c++)
    {
        uint8_t bus, dev, fn;
        usb::get_controller_location(c, &bus, &dev, &fn);
        uint8_t ports = usb::get_port_count(c);
        screen::printf("\n\r Controller %u (%u:%u.%u): %u root ports, context entry %u bytes",
                       (uint32_t)c, (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
                       (uint32_t)ports, usb::get_context_entry_size(c));
        if (ports == 0)
            screen::printf("\n\r  not running");

        for (uint8_t i = 0; i < ports; i++)
        {
            uint32_t raw = usb::get_port_status(c, i);
            char hex[9];
            hex_to_str(raw, hex, 8);

            screen::printf("\n\r  [%u] %s  0x%s  ccs=%u ped=%u pp=%u pr=%u pls=%u spd=%u",
                (uint32_t)i,
                usb::port_is_usb3(c, i) ? "usb3" : "usb2",
                hex,
                raw & 1, (raw >> 1) & 1, (raw >> 9) & 1, (raw >> 4) & 1,
                (raw >> 5) & 0xF, (raw >> 10) & 0xF);
        }
    }
}

static void cmd_lsusb(int argc, const char** argv)
{
    uint8_t count = usb::get_device_count();
    screen::printf("\n\r");
    screen::printf("\n\r USB devices: %u", (uint32_t)count);
    screen::printf("\n\r");

    if (count == 0)
    {
        screen::printf("\n\r No devices found");
        return;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        usb_device_info info;
        if (usb::get_device_info(i, &info) != USB_OK)
            continue;

        char vid[5], pid[5];
        hex_to_str(info.vendor_id, vid, 4);
        hex_to_str(info.product_id, pid, 4);

        screen::printf("\n\r  [%u] %s:%s  ctrl=%u slot=%u port=%u  %s",
            (uint32_t)i, vid, pid,
            (uint32_t)info.controller,
            (uint32_t)info.slot_id, 
            (uint32_t)info.port_index, 
            usb::get_usb_speed_str(info.port_speed));
        screen::printf("\n\r      class=%s  driver=%s\n\r",
            usb::get_usb_class_name(info.device_class),
            info.driver ? info.driver : "none");
    }
}

static void cmd_usbinfo(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: usbinfo <index>");
        return;
    }

    uint8_t index = 0;
    for (int i = 0; argv[1][i] != '\0'; i++)
    {
        if (argv[1][i] < '0' || argv[1][i] > '9')
        {
            screen::printf("\n\rInvalid index");
            return;
        }
        index = index * 10 + (argv[1][i] - '0');
    }

    usb_device_info info;
    if (usb::get_device_info(index, &info) != USB_OK)
    {
        screen::printf("\n\rDevice %u not found", (uint32_t)index);
        return;
    }

    char vid[5], pid[5];
    hex_to_str(info.vendor_id, vid, 4);
    hex_to_str(info.product_id, pid, 4);

    char bcd[5];
    hex_to_str(info.bcd_usb, bcd, 4);

    screen::printf("\n\r");
    screen::printf("\n\r USB Device %u", (uint32_t)index);
    screen::printf("\n\r  Vendor ID:     0x%s", vid);
    screen::printf("\n\r  Product ID:    0x%s", pid);
    screen::printf("\n\r  USB Version:   %c.%c%c", bcd[1], bcd[2], bcd[3]);
    screen::printf("\n\r  Speed:         %s", usb::get_usb_speed_str(info.port_speed));
    uint8_t bus, dev, fn;
    usb::get_controller_location(info.controller, &bus, &dev, &fn);
    screen::printf("\n\r  Controller:    %u (%u:%u.%u)", (uint32_t)info.controller,
                   (uint32_t)bus, (uint32_t)dev, (uint32_t)fn);
    screen::printf("\n\r  Slot:          %u", (uint32_t)info.slot_id);
    screen::printf("\n\r  Port:          %u", (uint32_t)info.port_index);
    screen::printf("\n\r  Class:         %s (%x)", usb::get_usb_class_name(info.device_class), (uint32_t)info.device_class);
    screen::printf("\n\r  Subclass:      %x", (uint32_t)info.device_subclass);
    screen::printf("\n\r  Protocol:      %x", (uint32_t)info.device_protocol);
    screen::printf("\n\r  Driver:        %s", info.driver ? info.driver : "none");

    if (info.vendor_str[0] != '\0')
        screen::printf("\n\r  Vendor:        %s", info.vendor_str);
    if (info.product_str[0] != '\0')
        screen::printf("\n\r  Product:       %s", info.product_str);
}

// One lsblk row: the name, after `branch` for a partition.
static void lsblk_row(blkdev* d, const char* branch)
{
    char name[32];
    uint32_t n = 0;
    for (const char* c = branch; *c && n < sizeof(name) - 1; c++)
        name[n++] = *c;
    for (const char* c = d->name; *c && n < sizeof(name) - 1; c++)
        name[n++] = *c;
    name[n] = '\0';

    uint64_t total_mb = d->sector_count * d->sector_size / (1024 * 1024);
    screen::printf("\n\r %-12s %uB x %u", name, d->sector_size, (uint32_t)d->sector_count);
    if (total_mb > 1024)
        screen::printf("  size=%u GB", (uint32_t)(total_mb / 1024));
    else
        screen::printf("  size=%u MB", (uint32_t)total_mb);
    if (d->parent)
        screen::printf("  offset=%u", (uint32_t)d->lba_offset);
    if (block::gone(d))
        screen::printf("  (unplugged)");

    uart::printf("lsblk: %s %uB x %u offset %u\n", d->name,
                 d->sector_size, (uint32_t)d->sector_count, (uint32_t)d->lba_offset);
}

// Every disk, its partitions under it.
static void cmd_lsblk(int argc, const char** argv)
{
    (void)argc; (void)argv;
    screen::printf("\n\r");
    uart::printf("lsblk:\n");

    uint32_t count = block::count();
    if (count == 0)
    {
        screen::printf("\n\r No block devices found");
        uart::printf("lsblk: no block devices\n");
        return;
    }

    static const char mid[]  = { (char)0xC3, (char)0xC4, ' ', 0 };     // ├─
    static const char last[] = { (char)0xC0, (char)0xC4, ' ', 0 };     // └─
    for (uint32_t i = 0; i < count; i++)
    {
        blkdev* disk = block::get(i);
        if (!disk || disk->parent)
            continue;
        lsblk_row(disk, "");
        blkdev* prev = nullptr;         // printed once the next one shows it is not the last
        for (uint32_t j = 0; j < count; j++)
        {
            blkdev* d = block::get(j);
            if (!d || d->parent != disk)
                continue;
            if (prev)
                lsblk_row(prev, mid);
            prev = d;
        }
        if (prev)
            lsblk_row(prev, last);
    }
}

static void cmd_meminfo(int argc, const char** argv)
{
    screen::printf("\n\r");

    uint64_t total_ram = memory::total();
    uint64_t total_ram_mb = total_ram / (1024 * 1024);
    screen::printf("\n\r Physical RAM:      %u MB", (uint32_t)total_ram_mb);

    pmm::Stats pstats;
    pmm::get_stats(&pstats);
    // Mirrored to the serial log: the test harness compares the free-frame
    // count across sessions to catch a leaked per-process kernel stack.
    uart::printf("meminfo: frames_free=%u frames_used=%u\n",
                 (uint32_t)pstats.free_frames, (uint32_t)pstats.used_frames);
    screen::printf("\n\r");
    screen::printf("\n\r PMM frames (4 KB): %u total, %u free, %u used",
        (uint32_t)pstats.total_frames, (uint32_t)pstats.free_frames, (uint32_t)pstats.used_frames);
    screen::printf("\n\r PMM managed:       %u MB", (uint32_t)(pstats.max_phys / (1024 * 1024)));

    HeapStats stats;
    heap::get_stats(&stats);

    uint32_t total_kb = (uint32_t)(stats.total_size / 1024);
    uint32_t used_kb  = (uint32_t)(stats.used_size / 1024);
    uint32_t free_kb  = (uint32_t)(stats.free_size / 1024);

    screen::printf("\n\r");
    screen::printf("\n\r Heap total:        %u KB", total_kb);
    screen::printf("\n\r Heap used:         %u KB", used_kb);
    screen::printf("\n\r Heap free:         %u KB", free_kb);
    screen::printf("\n\r Largest free:      %u KB", (uint32_t)(stats.largest_free_block / 1024));
    // The heap is a list of pieces: each allocation is one, and the free
    // room between them is split into gaps.
    screen::printf("\n\r Heap allocations:  %u, free room in %u piece(s)",
                   (uint32_t)stats.used_block_count, (uint32_t)stats.free_block_count);
}

static void cmd_acpi(int argc, const char** argv)
{
    (void)argc; (void)argv;

    uint32_t units = 0, disabled = 0, flags = 0;
    acpi::dmar_status(&units, &disabled, &flags);

    if (!acpi::available())
    {
        screen::printf("\n\rNo ACPI tables");
        return;
    }

    char sig[5];
    uint64_t phys = 0;
    uint32_t len = 0;
    bool ok = false;
    for (uint32_t i = 0; acpi::table_info(i, sig, &phys, &len, &ok); i++)
        screen::printf("\n\r%s at %llx, %u bytes%s", sig, phys, len, ok ? "" : ", BAD CHECKSUM");

    uint8_t a = 0, b = 0;
    if (acpi::s5_values(&a, &b))
        screen::printf("\n\r_S5: SLP_TYPa=%u SLP_TYPb=%u", (uint32_t)a, (uint32_t)b);
    else
        screen::printf("\n\r_S5: not found (shutdown unavailable)");
    screen::printf("\n\rReset register: %s%s",
                   acpi::has_reset_register() ? "yes" : "no",
                   acpi::hardware_reduced() ? ", hardware-reduced ACPI" : "");

    const acpi::madt_info* m = acpi::madt();
    if (!m->present)
        screen::printf("\n\rMADT: none (no APIC information)");
    else
    {
        screen::printf("\n\rCPUs: %u, local APIC at %llx%s", m->cpu_count, m->lapic_address,
                       m->has_8259 ? ", 8259 PICs present too" : "");
        for (uint32_t i = 0; i < m->cpu_count; i++)
            screen::printf("\n\r  cpu %u: APIC id %u%s", i, m->cpus[i].apic_id,
                           m->cpus[i].enabled ? "" : " (can be started)");
        for (uint32_t i = 0; i < m->ioapic_count; i++)
            screen::printf("\n\rIOAPIC id %u at %llx, GSI from %u", m->ioapics[i].id,
                           m->ioapics[i].address, m->ioapics[i].gsi_base);
        for (uint32_t i = 0; i < m->override_count; i++)
            screen::printf("\n\r  IRQ %u -> GSI %u%s%s", (uint32_t)m->overrides[i].irq,
                           m->overrides[i].gsi, m->overrides[i].active_low ? ", active low" : "",
                           m->overrides[i].level ? ", level" : "");
    }

    if (flags & BOOT_DMAR_PRESENT)
        screen::printf("\n\rVT-d: %u unit(s), remapping was %s, turned off on %u%s",
                       units, (flags & BOOT_DMAR_WAS_ENABLED) ? "on" : "off", disabled,
                       (flags & BOOT_DMAR_TIMEOUT) ? " (TIMEOUT)" : "");
    else
        screen::printf("\n\rVT-d: no DMAR table");
}

namespace reports
{
    bool report(const char* topic)
    {
        // The topic and its arguments, split at spaces.
        char line[64];
        strncpy(line, topic, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        const char* argv[4];
        int argc = 0;
        for (char* p = line; *p && argc < 4;)
        {
            while (*p == ' ')
                *p++ = '\0';
            if (!*p)
                break;
            argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
        }
        if (argc == 0)
            return false;

        static const struct { const char* name; void (*fn)(int, const char**); } topics[] = {
            { "cpuid", cmd_cpuid },   { "lspci", cmd_lspci },       { "lsusb", cmd_lsusb },
            { "usbports", cmd_usbports }, { "usbinfo", cmd_usbinfo }, { "lsblk", cmd_lsblk },
            { "mount", cmd_mount },   { "acpi", cmd_acpi },         { "meminfo", cmd_meminfo },
            { "dmesg", cmd_dmesg },
        };
        for (const auto& t : topics)
        {
            if (strcmp(argv[0], t.name) != 0)
                continue;
            // "mount" alone only lists.
            t.fn(strcmp(argv[0], "mount") == 0 ? 1 : argc, argv);
            return true;
        }
        return false;
    }
}
