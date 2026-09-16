#include "../include/stdio.h"
#include "../include/abi/syscall.h"

// raw syscall
extern uint64_t syscall(uint64_t num, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0);

void print(const char* str)
{
    syscall(SYS_WRITE, (uint64_t)str, 0, 0);
}

void print_u64(uint64_t value)
{
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do
    {
        buf[--pos] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(buf + pos);
}

void print_i64(sint64_t value)
{
    if (value < 0)
    {
        print("-");
        print_u64((uint64_t)0 - (uint64_t)value);
        return;
    }
    print_u64((uint64_t)value);
}

void print_hex64(uint64_t value)
{
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 18;
    buf[pos] = '\0';
    do
    {
        uint8_t d = value & 0xF;
        buf[--pos] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value >>= 4;
    } while (value);
    while (pos > 2)
        buf[--pos] = '0';
    print(buf);
}

void clear() 
{
    syscall(SYS_CLEAR, 0, 0, 0);
}

void set_cursor(uint32_t x, uint32_t y)
{
    syscall(SYS_SET_CURSOR, (uint64_t)x, (uint64_t)y, 0);
}

keyboard_event_t read_key()
{
    keyboard_event_t e;
    syscall(SYS_READ_KEY, (uint64_t)&e, 0, 0);
    return e;
}

uint32_t read_line(char* buffer, uint32_t max_len)
{
    return (uint32_t)syscall(SYS_READ_LINE, (uint64_t)buffer, (uint64_t)max_len, 0);
}

void exit(int code)
{
    syscall(SYS_EXIT, (uint64_t)code, 0, 0);
    while (1) {}
}

// File I/O

uint32_t write_file(const char* path, const uint8_t* data, uint32_t size)
{
    return (uint32_t)syscall(SYS_WRITE_FILE, (uint64_t)path, (uint64_t)data, (uint64_t)size);
}

uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size)
{
    return (uint32_t)syscall(SYS_READ_FILE, (uint64_t)path, (uint64_t)buffer, (uint64_t)max_size);
}

uint32_t stat_file(const char* path, file_stat_t* out)
{
    return (uint32_t)syscall(SYS_STAT_FILE, (uint64_t)path, (uint64_t)out, 0);
}

uint32_t read_dir(const char* path, dir_entry_t* entries, uint32_t max_entries)
{
    return (uint32_t)syscall(SYS_READ_DIR, (uint64_t)path, (uint64_t)entries, (uint64_t)max_entries);
}

// Time

void get_uptime(uptime_t* out)
{
    syscall(SYS_UPTIME, (uint64_t)out, 0, 0);
}

void get_time(datetime_t* out)
{
    syscall(SYS_TIME, (uint64_t)out, 0, 0);
}