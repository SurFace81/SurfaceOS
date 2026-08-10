#include <stdio.h>
#include <string.h>

static void print_uint(uint32_t n)
{
    char buf[12];
    int pos = 0;

    if (n == 0)
    {
        print("0");
        return;
    }

    while (n > 0)
    {
        buf[pos++] = '0' + (n % 10);
        n /= 10;
    }

    char out[12];
    for (int i = 0; i < pos; i++)
        out[i] = buf[pos - 1 - i];
    out[pos] = '\0';
    print(out);
}

static void print_pad2(uint32_t n)
{
    if (n < 10)
        print("0");
    print_uint(n);
}

static const char* weekday_name(uint8_t wd)
{
    switch (wd)
    {
        case 1: return "Sun";
        case 2: return "Mon";
        case 3: return "Tue";
        case 4: return "Wed";
        case 5: return "Thu";
        case 6: return "Fri";
        case 7: return "Sat";
        default: return "???";
    }
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        print("Args: ");
        for (int i = 1; i < argc; i++)
        {
            print(argv[i]);
            print(" ");
        }
        print("\n\n");
    }

    // Test SYS_UPTIME
    print("=== UPTIME ===\n");
    uptime_t up;
    get_uptime(&up);
    print("Up: ");
    print_uint(up.hours);
    print("h ");
    print_uint(up.minutes);
    print("m ");
    print_uint(up.seconds);
    print("s (");
    print_uint((uint32_t)(up.total_ms / 1000));
    print(" sec total)\n\n");

    // Test SYS_TIME
    print("=== TIME ===\n");
    datetime_t dt;
    get_time(&dt);
    print(weekday_name(dt.weekday));
    print(" ");
    print_pad2(dt.day);
    print(".");
    print_pad2(dt.month);
    print(".");
    print_uint(dt.year);
    print(" ");
    print_pad2(dt.hours);
    print(":");
    print_pad2(dt.minutes);
    print(":");
    print_pad2(dt.seconds);
    print("\n\n");

    // Test SYS_STAT_FILE
    print("=== STAT FILE ===\n");
    print("Path: ");
    char path[128];
    read_line(path, sizeof(path));

    file_stat_t st;
    uint32_t res = stat_file(path, &st);
    if (res == (uint32_t)-1)
    {
        print("File not found\n\n");
    }
    else
    {
        print("Size: ");
        print_uint(st.size);
        print(" bytes\n");

        print("Attr: ");
        if (st.attr & FILE_ATTR_DIRECTORY) print("[DIR] ");
        if (st.attr & FILE_ATTR_READ_ONLY) print("[RO] ");
        if (st.attr & FILE_ATTR_HIDDEN)    print("[HID] ");
        if (st.attr & FILE_ATTR_SYSTEM)    print("[SYS] ");
        if (st.attr & FILE_ATTR_ARCHIVE)   print("[ARC] ");
        print("\n\n");
    }

    // Test SYS_READ_DIR
    print("=== READ DIR ===\n");
    print("Dir path (empty for cwd): ");
    char dir_path[128];
    uint32_t dir_len = read_line(dir_path, sizeof(dir_path));

    const uint32_t max = 32;
    dir_entry_t entries[max];
    uint32_t count = read_dir(dir_len > 0 ? dir_path : "\\", entries, max);

    if (count == 0)
    {
        print("Empty or not found\n");
    }
    else
    {
        print_uint(count);
        print(" entries:\n");

        for (uint32_t i = 0; i < count; i++)
        {
            if (entries[i].attr & FILE_ATTR_DIRECTORY)
                print("  <DIR>  ");
            else
            {
                print("  ");
                print_uint(entries[i].size);
                print("  ");
            }
            print(entries[i].name);
            print("\n");
        }
    }

    print("\nDone!\n");
    return 0;
}