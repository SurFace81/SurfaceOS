#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/syscall.h"
#include "../include/abi/syscall.h"
#include "../include/abi/time.h"

static uint32_t str_len(const char* s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

// stdout: byte-exact write(1, ...). Not a C-string call: the length comes
// from strlen(), and NULs inside a buffer pass through.
void print(const char* str)
{
    write(1, str, str_len(str));
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
    syscall(SYSX_CLEAR);
}

void set_cursor(uint32_t x, uint32_t y)
{
    syscall(SYSX_SET_CURSOR, (uint64_t)x, (uint64_t)y);
}

keyboard_event_t read_key()
{
    keyboard_event_t e;
    syscall(SYSX_READ_KEY, (uint64_t)&e);
    return e;
}

// One canonical tty read of fd 0: returns the line without '\n', NUL-
// terminated. 0 on EOF (Ctrl+D on an empty line).
uint32_t read_line(char* buffer, uint32_t max_len)
{
    if (max_len == 0)
        return 0;

    ssize_t n = read(0, buffer, max_len - 1);
    if (n <= 0)
    {
        buffer[0] = '\0';
        return 0;
    }

    buffer[n] = '\0';
    if (n > 0 && buffer[n - 1] == '\n')
    {
        buffer[n - 1] = '\0';
        n--;
    }
    return (uint32_t)n;
}

void exit(int code)
{
    syscall(SYS_EXIT_GROUP, (uint64_t)code);
    while (1) {}
}

// Time

void get_uptime(uptime_t* out)
{
    syscall(SYSX_UPTIME, (uint64_t)out);
}

void get_time(datetime_t* out)
{
    syscall(SYSX_TIME, (uint64_t)out);
}
