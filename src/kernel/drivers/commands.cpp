// src/kernel/drivers/commands.cpp
#include "../../include/drivers/commands.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/usb/xhci.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/rtc.h"
#include "../../include/cpu/program.h"

// Built-in commands

static void cmd_help(int argc, const char** argv)
{
    uint32_t cols = screen::cols();

    uint32_t col_width = 16;
    uint32_t num_cols = cols / col_width;
    if (num_cols < 1)
        num_cols = 1;

    uint32_t count = console::command_count();

    screen::printf("\n\r");
    for (uint32_t i = 0; i < count; i++)
    {
        if (i % num_cols == 0 && i > 0)
            screen::printf("\n\r");

        const char* name = console::command_name(i);
        screen::printf(" %s", name);

        uint32_t name_len = strlen(name);
        uint32_t pad = col_width - name_len - 1;
        for (uint32_t p = 0; p < pad; p++)
            screen::printf(" ");
    }
}

static void cmd_cls(int argc, const char** argv)
{
    screen::clear();
    screen::show_cursor();
}

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
    screen::printf("\n\r Base freq:      %u MHz", cpuid::get_base_freq());
    screen::printf("\n\r Max freq:       %u MHz", cpuid::get_max_freq());
    screen::printf("\n\r Bus freq:       %u MHz", cpuid::get_bus_freq());
    screen::printf("\n\r Logical cores:  %u", topo.logical_cores);
    screen::printf("\n\r Physical cores: %u", topo.physical_cores);
    screen::printf("\n\r Sockets:        %u", topo.packages);
    screen::printf("\n\r Hyperthreading: %s", topo.hyperthreading ? "Yes" : "No");
    screen::printf("\n\r L1 cache:       %u KB", cache.l1d_size + cache.l1i_size);
    screen::printf("\n\r L2 cache:       %u KB", cache.l2_size);
    screen::printf("\n\r L3 cache:       %u KB", cache.l3_size);
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

static void cmd_mount(int argc, const char** argv)
{
    uint8_t index = 0;

    if (argc > 1)
    {
        for (int i = 0; argv[1][i] != '\0'; i++)
        {
            if (argv[1][i] < '0' || argv[1][i] > '9')
            {
                screen::printf("\n\rUsage: mount [device_index]");
                return;
            }
            index = index * 10 + (argv[1][i] - '0');
        }
    }

    if (fat32::mount(index))
        screen::printf("\n\rFAT32 mounted (device %u)", (uint32_t)index);
    else
        screen::printf("\n\rMount failed");
}

static void cmd_umount(int argc, const char** argv)
{
    if (!fat32::is_mounted())
    {
        screen::printf("\n\rNothing is mounted");
        return;
    }

    fat32::umount();
    screen::printf("\n\rUnmounted");
}

static void cmd_ls(int argc, const char** argv)
{
    const char* path = argc > 1 ? argv[1] : "";

    const uint32_t max_entries = 64;
    fat32_dir_entry entries[max_entries];

    uint32_t count = fat32::ls(path, entries, max_entries);

    if (count == 0 && argc > 1)
    {
        screen::printf("\n\rDirectory not found: %s", argv[1]);
        return;
    }

    for (uint32_t i = 0; i < count && i < max_entries; i++)
    {
        char name[13];
        format_83_name(entries[i].name, name);

        char dt[17];
        format_datetime(entries[i].write_date, entries[i].write_time, dt);

        if (entries[i].attr & FAT32_ATTR_DIRECTORY)
            screen::printf("\n\r  %s       <DIR>  %s", dt, name);
        else
            screen::printf("\n\r  %s  %10u  %s", dt, entries[i].file_size, name);
    }
}

static void cmd_copy(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: copy <source> <destination>");
        return;
    }

    screen::printf("\n\r");
    if (fat32::copy(argv[1], argv[2]))
        screen::printf("Copied %s -> %s", argv[1], argv[2]);
    else
        screen::printf("Copy failed");
}

static void cmd_rename(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: rename <path> <new_name>");
        return;
    }

    screen::printf("\n\r");
    if (fat32::rename(argv[1], argv[2]))
        screen::printf("Renamed %s -> %s", argv[1], argv[2]);
    else
        screen::printf("Rename failed");
}

static bool is_printable(uint8_t c)
{
    return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
}

static void cmd_cat(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: cat <filename>");
        return;
    }

    const uint32_t max_size = 512;
    uint8_t* buf = (uint8_t*)kmalloc(max_size);
    if (!buf)
    {
        screen::printf("\n\rOut of memory");
        return;
    }

    uint32_t n = fat32::read_file(argv[1], buf, max_size);
    if (n == (uint32_t)-1 || n == 0)
    {
        screen::printf("\n\rFile not found or read error");
        kfree(buf);
        return;
    }

    // Detect binary content
    bool binary = false;
    for (uint32_t i = 0; i < n; i++)
    {
        if (!is_printable(buf[i]))
        {
            binary = true;
            break;
        }
    }

    if (binary)
    {
        screen::printf("\n\rBinary file (%u bytes), use xxd to view", n);
        kfree(buf);
        return;
    }

    screen::printf("\n\r");
    buf[n] = 0;
    screen::printf("%s", (char*)buf);

    if (n == max_size)
    {
        screen::printf("\n\r[truncated at %u bytes]", max_size);
    }
    else 
    {
        screen::printf("\n\r%u bytes", n);
    }

    kfree(buf);
}

static void cmd_xxd(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: xxd <filename>");
        return;
    }

    const uint32_t max_size = 512;
    uint8_t* buf = (uint8_t*)kmalloc(max_size);
    if (!buf)
    {
        screen::printf("\n\rOut of memory");
        return;
    }

    uint32_t n = fat32::read_file(argv[1], buf, max_size);
    if (n == (uint32_t)-1 || n == 0)
    {
        screen::printf("\n\rFile not found or read error");
        kfree(buf);
        return;
    }

    screen::printf("\n\r");

    const char* hex = "0123456789ABCDEF";

    for (uint32_t off = 0; off < n; off += 16)
    {
        // Offset (8 hex digits)
        char addr[9];
        for (int i = 7; i >= 0; i--)
            addr[i] = hex[(off >> ((7 - i) * 4)) & 0xF];
        addr[8] = '\0';
        screen::printf("%s  ", addr);

        // Hex bytes
        for (uint32_t i = 0; i < 16; i++)
        {
            if (off + i < n)
            {
                uint8_t b = buf[off + i];
                screen::printf("%c%c ", hex[b >> 4], hex[b & 0xF]);
            }
            else
            {
                screen::printf("   ");
            }

            if (i == 7) screen::printf(" ");
        }

        // ASCII column
        screen::printf(" |");
        for (uint32_t i = 0; i < 16 && off + i < n; i++)
        {
            uint8_t b = buf[off + i];
            if (b >= 0x20 && b <= 0x7E)
                screen::printf("%c", b);
            else
                screen::printf(".");
        }
        screen::printf("|\n\r");
    }

    if (n == max_size) 
    {
        screen::printf("[truncated at %u bytes]\n\r", max_size);
    }
    else 
    {
        screen::printf("\n\r%u bytes", n);
    }   

    kfree(buf);
}

static void cmd_write(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: write <filename> <text...>");
        return;
    }

    // Concatenate all args after filename
    char data[512];
    uint32_t pos = 0;
    for (int i = 2; i < argc && pos < 510; i++)
    {
        if (i > 2 && pos < 510)
            data[pos++] = ' ';

        uint32_t len = strlen(argv[i]);
        for (uint32_t j = 0; j < len && pos < 510; j++)
            data[pos++] = argv[i][j];
    }
    data[pos] = '\0';

    uint32_t written = fat32::write_file(argv[1], (const uint8_t*)data, pos);
    screen::printf("\n\r");
    if (written != (uint32_t)-1)
        screen::printf("%u bytes written to %s", written, argv[1]);
    else
        screen::printf("Write failed");
}

static void cmd_mkdir(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: mkdir <dirname>");
        return;
    }

    screen::printf("\n\r");
    if (fat32::mkdir(argv[1]))
        screen::printf("Directory created: %s", argv[1]);
    else
        screen::printf("mkdir failed");
}

static void cmd_rm(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: rm <path>");
        return;
    }

    screen::printf("\n\r");
    if (fat32::remove(argv[1]))
        screen::printf("Removed: %s", argv[1]);
    else
        screen::printf("Remove failed");
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

        screen::printf("\n\r  [%u] %s:%s  slot=%u port=%u  %s",
            (uint32_t)i, vid, pid,
            (uint32_t)info.slot_id, 
            (uint32_t)info.port_index, 
            usb::get_usb_speed_str(info.port_speed));
        screen::printf("\n\r      class=%s  %s\n\r", 
            usb::get_usb_class_name(info.device_class),
            info.is_mass_storage ? "[mass storage]" : "");
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
    screen::printf("\n\r  Slot:          %u", (uint32_t)info.slot_id);
    screen::printf("\n\r  Port:          %u", (uint32_t)info.port_index);
    screen::printf("\n\r  Class:         %s (0x%x)", usb::get_usb_class_name(info.device_class), (uint32_t)info.device_class);
    screen::printf("\n\r  Subclass:      0x%x", (uint32_t)info.device_subclass);
    screen::printf("\n\r  Protocol:      0x%x", (uint32_t)info.device_protocol);
    screen::printf("\n\r  Mass Storage:  %s", info.is_mass_storage ? "Yes" : "No");
    screen::printf("\n\r  Connected:     %s", info.connected ? "Yes" : "No");

    if (info.vendor_str[0] != '\0')
        screen::printf("\n\r  Vendor:        %s", info.vendor_str);
    if (info.product_str[0] != '\0')
        screen::printf("\n\r  Product:       %s", info.product_str);

    if (info.is_mass_storage)
    {
        uint8_t blk_count = usb::get_block_device_count();
        for (uint8_t b = 0; b < blk_count; b++)
        {
            usb_block_device bdev;
            if (usb::get_block_device_info(b, &bdev) != USB_OK)
                continue;

            screen::printf("\n\r  Block device:");
            screen::printf("\n\r    Block size:  %u bytes", bdev.block_size);
            screen::printf("\n\r    Last LBA:    %u", bdev.last_lba);

            uint64_t total_mb = bdev.total_bytes / (1024 * 1024);
            if (total_mb > 1024)
                screen::printf("\n\r    Capacity:    %u GB", (uint32_t)(total_mb / 1024));
            else
                screen::printf("\n\r    Capacity:    %u MB", (uint32_t)total_mb);

            screen::printf("\n\r    Ready:       %s", bdev.ready ? "Yes" : "No");
            break;
        }
    }
}

static void cmd_lsblk(int argc, const char** argv)
{
    uint8_t count = usb::get_block_device_count();
    screen::printf("\n\r");
    screen::printf("\n\r Block devices: %u", (uint32_t)count);
    screen::printf("\n\r");

    if (count == 0)
    {
        screen::printf("\n\r No block devices found");
        return;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        usb_block_device bdev;
        if (usb::get_block_device_info(i, &bdev) != USB_OK)
            continue;

        uint64_t total_mb = bdev.total_bytes / (1024 * 1024);

        screen::printf("\n\r  [%u] block_size=%u  sectors=%u",
            (uint32_t)i, bdev.block_size, bdev.last_lba + 1);

        if (total_mb > 1024)
            screen::printf("  size=%u GB", (uint32_t)(total_mb / 1024));
        else
            screen::printf("  size=%u MB", (uint32_t)total_mb);

        screen::printf("  %s", bdev.ready ? "ready" : "not ready");
    }
}

static void cmd_meminfo(int argc, const char** argv)
{
    screen::printf("\n\r");

    uint64_t total_ram = memory::total();
    uint64_t total_ram_mb = total_ram / (1024 * 1024);
    screen::printf("\n\r Physical RAM:      %u MB", (uint32_t)total_ram_mb);

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
    screen::printf("\n\r");
    screen::printf("\n\r Blocks total:      %u", (uint32_t)stats.block_count);
    screen::printf("\n\r Blocks used:       %u", (uint32_t)stats.used_block_count);
    screen::printf("\n\r Blocks free:       %u", (uint32_t)stats.free_block_count);
}

static void cmd_time(int argc, const char** argv)
{
    rtc_time t;
    rtc::read(&t);

    screen::printf("\n\r");
    screen::printf("\n\r %02u:%02u:%02u  %02u.%02u.%u",
        (uint32_t)t.hours, (uint32_t)t.minutes, (uint32_t)t.seconds,
        (uint32_t)t.day, (uint32_t)t.month, (uint32_t)t.year);
}

static void cmd_uptime(int argc, const char** argv)
{
    uint64_t ms = pit::uptime_ms();

    uint32_t total_sec = (uint32_t)(ms / 1000);
    uint32_t hours = total_sec / 3600;
    uint32_t minutes = (total_sec % 3600) / 60;
    uint32_t seconds = total_sec % 60;
    uint32_t millis = (uint32_t)(ms % 1000);

    screen::printf("\n\r");
    screen::printf("\n\r Uptime: %u:%02u:%02u.%03u", hours, minutes, seconds, millis);
    screen::printf("\n\r Ticks:  %u", (uint32_t)pit::ticks());
    screen::printf("\n\r Freq:   %u Hz (real: %u Hz)", pit::frequency(), pit::real_frequency());
}

static void cmd_settime(int argc, const char** argv)
{
    // Usage: settime HH:MM:SS [DD.MM.YYYY]
    if (argc < 2)
    {
        screen::printf("\n\r Usage: settime HH:MM:SS [DD.MM.YYYY]");
        return;
    }

    // Parse time: HH:MM:SS
    const char* ts = argv[1];

    // Validate minimum length "H:M:S" = 5 chars
    uint32_t len = strlen(ts);
    if (len < 5)
    {
        screen::printf("\n\r Invalid time format");
        return;
    }

    // Read current RTC as base values
    rtc_time t;
    rtc::read(&t);

    // Find colons and parse
    char time_buf[16];
    if (len > 15) len = 15;
    memcpy(time_buf, ts, len);
    time_buf[len] = '\0';

    // Replace ':' with '\0' to split
    char* parts[3];
    uint32_t part_count = 0;
    parts[part_count++] = time_buf;
    for (uint32_t i = 0; i < len && part_count < 3; i++)
    {
        if (time_buf[i] == ':')
        {
            time_buf[i] = '\0';
            parts[part_count++] = &time_buf[i + 1];
        }
    }

    if (part_count < 3)
    {
        screen::printf("\n\r Invalid time format, use HH:MM:SS");
        return;
    }

    t.hours   = (uint8_t)parse_uint(parts[0]);
    t.minutes = (uint8_t)parse_uint(parts[1]);
    t.seconds = (uint8_t)parse_uint(parts[2]);

    // Validate
    if (t.hours > 23 || t.minutes > 59 || t.seconds > 59)
    {
        screen::printf("\n\r Invalid time values");
        return;
    }

    // Parse optional date: DD.MM.YYYY
    if (argc >= 3)
    {
        const char* ds = argv[2];
        uint32_t dlen = strlen(ds);

        char date_buf[16];
        if (dlen > 15) dlen = 15;
        memcpy(date_buf, ds, dlen);
        date_buf[dlen] = '\0';

        char* dparts[3];
        uint32_t dpart_count = 0;
        dparts[dpart_count++] = date_buf;
        for (uint32_t i = 0; i < dlen && dpart_count < 3; i++)
        {
            if (date_buf[i] == '.')
            {
                date_buf[i] = '\0';
                dparts[dpart_count++] = &date_buf[i + 1];
            }
        }

        if (dpart_count < 3)
        {
            screen::printf("\n\r Invalid date format, use DD.MM.YYYY");
            return;
        }

        t.day   = (uint8_t)parse_uint(dparts[0]);
        t.month = (uint8_t)parse_uint(dparts[1]);
        t.year  = (uint16_t)parse_uint(dparts[2]);

        if (t.day < 1 || t.day > 31 || t.month < 1 || t.month > 12)
        {
            screen::printf("\n\r Invalid date values");
            return;
        }
    }

    rtc::write(&t);

    screen::printf("\n\r Time set to %02u:%02u:%02u", (uint32_t)t.hours, (uint32_t)t.minutes, (uint32_t)t.seconds);

    if (argc >= 3) 
    {
        screen::printf("  %02u.%02u.%u", (uint32_t)t.day, (uint32_t)t.month, (uint32_t)t.year);
    }        
}

static void cmd_cd(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\r%s", fat32::cwd_path());
        return;
    }

    if (!fat32::set_cwd(argv[1]))
        screen::printf("\n\rDirectory not found: %s", argv[1]);
}

static void cmd_exec(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: exec <filename>");
        return;
    }

    screen::printf("\n\r");
    if (!program::exec(argv[1]))
        screen::printf("Failed to load: %s", argv[1]);
}

namespace commands
{
    void init()
    {
        console::register_command("help",    cmd_help);
        console::register_command("cls",     cmd_cls);
        console::register_command("cpuid",   cmd_cpuid);
        console::register_command("lspci",   cmd_lspci);
        console::register_command("mount",   cmd_mount);
        console::register_command("umount",  cmd_umount);
        console::register_command("ls",      cmd_ls);
        console::register_command("cat",     cmd_cat);
        console::register_command("xxd",     cmd_xxd);
        console::register_command("write",   cmd_write);
        console::register_command("mkdir",   cmd_mkdir);
        console::register_command("rm",      cmd_rm);
        console::register_command("lsusb",   cmd_lsusb);
        console::register_command("usbinfo", cmd_usbinfo);
        console::register_command("lsblk",   cmd_lsblk);
        console::register_command("meminfo", cmd_meminfo);
        console::register_command("time",    cmd_time);
        console::register_command("uptime",  cmd_uptime);
        console::register_command("settime", cmd_settime);
        console::register_command("cd",      cmd_cd);
        console::register_command("copy",    cmd_copy);
        console::register_command("rename",  cmd_rename);
        console::register_command("exec",    cmd_exec);
    }
}