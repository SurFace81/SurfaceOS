#include "../../include/drivers/commands.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/stdlib/string.h"
#include "../version.h"

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

static void cmd_clear(int argc, const char** argv)
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
    if (fat32::mount(0))
        screen::printf("\n\rFAT32 mounted");
    else
        screen::printf("\n\rMount failed");
}

static void cmd_ls(int argc, const char** argv)
{
    const char* path = argc > 1 ? argv[1] : "/";
    screen::printf("\n\r");
    fat32::ls(path);
}

static void cmd_cat(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: cat <filename>");
        return;
    }

    uint8_t buf[1024];
    uint32_t n = fat32::read_file(argv[1], buf, 1023);
    screen::printf("\n\r");
    if (n > 0)
    {
        buf[n] = 0;
        screen::printf("%s", (char*)buf);
    }
    else
    {
        screen::printf("File not found or read error");
    }
}

namespace commands
{
    void init()
    {
        console::register_command("help",  cmd_help);
        console::register_command("clear", cmd_clear);
        console::register_command("cpuid", cmd_cpuid);
        console::register_command("lspci", cmd_lspci);
        console::register_command("mount", cmd_mount);
        console::register_command("ls",    cmd_ls);
        console::register_command("cat",   cmd_cat);
    }
} // namespace commands