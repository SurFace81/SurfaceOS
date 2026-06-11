#include "../../include/drivers/console.h"
#include "../../include/drivers/uart.h"

#define MAX_COMMANDS 32

struct Command
{
    const char* name;
    uint32_t name_len;
    command_fn handler;
};

static Command cmd_table[MAX_COMMANDS];
static uint32_t cmd_count = 0;

static list::List<char>* input_buf;

// helpers
static uint32_t str_len(const char* s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool buf_eq(list::List<char>* buf, const char* str, uint32_t len)
{
    if (list::size(buf) != len)
        return false;
    for (uint32_t i = 0; i < len; i++)
    {
        char c;
        list::get(buf, i, c);
        if (c != str[i])
            return false;
    }
    return true;
}

// built-in commands
static void cmd_help(list::List<char>*)
{
    screen::printf("\n\r");
    for (uint32_t i = 0; i < cmd_count; i++)
        screen::printf(" %s\n\r", cmd_table[i].name);
}

static void cmd_clear(list::List<char>*)
{
    screen::clear();
}

static void cmd_cpuid(list::List<char>*)
{
    char name[64];
    cpuid::get_cpu_name(name);

    CPUTopology topo;
    cpuid::get_cpu_topology(&topo);

    CacheInfo cache;
    cpuid::get_cache_info(&cache);

    screen::printf("\n\r           CPU: %s", name);
    screen::printf("\n\r      BaseFreq: %u MHz", cpuid::get_base_freq());
    screen::printf("\n\r       MaxFreq: %u MHz", cpuid::get_max_freq());
    screen::printf("\n\r       BusFreq: %u MHz", cpuid::get_bus_freq());
    screen::printf("\n\r Logical cores: %u", topo.logical_cores);
    screen::printf("\n\rPhysical cores: %u", topo.physical_cores);
    screen::printf("\n\r       Sockets: %u", topo.packages);
    screen::printf("\n\rHyperthreading: %s", topo.hyperthreading ? "Yes" : "No");
    screen::printf("\n\r            L1: %u KB", cache.l1d_size + cache.l1i_size);
    screen::printf("\n\r            L2: %u KB", cache.l2_size);
    screen::printf("\n\r            L3: %u KB", cache.l3_size);
}

static void cmd_lspci(list::List<char>*)
{
    uint32_t count = pci::device_count();
    screen::printf("\n\r PCI devices: %u", count);

    for (uint32_t i = 0; i < count; i++)
    {
        PCIDevice* d = pci::get_by_id(i);
        screen::printf("\n\r %x:%x class %x:%x prog %x", (uint32_t)d->vendor_id, (uint32_t)d->device_id,
                       (uint32_t)d->class_code, (uint32_t)d->subclass, (uint32_t)d->prog_if);
    }
}

// input handler
static void exec(list::List<char>* buf)
{
    if (list::size(buf) == 0)
        return;

    for (uint32_t i = 0; i < cmd_count; i++)
    {
        if (buf_eq(buf, cmd_table[i].name, cmd_table[i].name_len))
        {
            cmd_table[i].handler(buf);
            return;
        }
    }

    screen::printf("\n\rUnknown command");
}

static void on_key(keyboard_event_t e)
{
    if (e.type != KEY_PRESS)
        return;

    if (e.KeyCode == Keys::BACKSPACE)
    {
        if (list::size(input_buf) == 0)
            return;

        list::remove_at(input_buf, list::size(input_buf) - 1);

        // Move cursor back and erase
        uint32_t cx = screen::cursor_x();
        uint32_t cy = screen::cursor_y();

        if (cx > 0)
            cx--;
        else if (cy > 0)
        {
            cy--;
            cx = screen::cols() - 1;
        }

        screen::erase_at(cx, cy);
        screen::set_cursor(cx, cy);
        return;
    }

    if (e.KeyCode == Keys::ENTER)
    {
        exec(input_buf);
        list::clear(input_buf);
        screen::printf("\n\r> ");
        return;
    }

    // Regular character
    list::add(input_buf, e.KeyChar);
    char out[2] = {e.KeyChar, '\0'};
    screen::write(out);
    uart::printf("%c", e.KeyChar);
}

// API
namespace console
{
    void register_command(const char* name, command_fn handler)
    {
        if (cmd_count >= MAX_COMMANDS)
            return;

        cmd_table[cmd_count].name       = name;
        cmd_table[cmd_count].name_len   = str_len(name);
        cmd_table[cmd_count].handler    = handler;
        cmd_count++;
    }

    void init()
    {
        cmd_count = 0; // .bss is not zeroed in flat binary — explicit init required
        input_buf = list::create<char>();

        // Register built-in commands
        register_command("help", cmd_help);
        register_command("clear", cmd_clear);
        register_command("cpuid", cmd_cpuid);
        register_command("lspci", cmd_lspci);

        keyboard::set_keyboard_callback(on_key);

        // Welcome message
        char cpu_name[51];
        cpuid::get_cpu_name(cpu_name);

        char freq[12];
        int_to_str(cpuid::get_base_freq(), freq);

        screen::printf("\n\tSurfaceOS v0.1 (C) 2025\n\r\tMem: ");
        screen::printf("%u", (uint32_t)(memory::total() / 1048576 + 1));
        screen::printf(" Mb\n\r\tCpu: %s @ %s MHz", cpu_name, freq);
        screen::printf("\n\r------------------------------------------------\n\n\r> ");
    }

} // namespace console