#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

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

int main()
{
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

    // stat(2) through the fd layer
    print("=== STAT FILE ===\n");
    print("Path: ");
    char path[128];
    read_line(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0)
    {
        print("stat failed, errno ");
        print_i64(errno);
        print("\n\n");
    }
    else
    {
        print("Size: ");
        print_u64((uint64_t)st.st_size);
        print(" bytes\n");

        print("Mode: ");
        print_hex64(st.st_mode);
        print(S_ISDIR(st.st_mode) ? " [DIR]" : " [REG]");
        if (!(st.st_mode & S_IWUSR)) print(" [RO]");
        print("\n\n");
    }

    // opendir/readdir through getdents64
    print("=== READ DIR ===\n");
    print("Dir path (empty for cwd): ");
    char dir_path[128];
    uint32_t dir_len = read_line(dir_path, sizeof(dir_path));

    DIR* d = opendir(dir_len > 0 ? dir_path : ".");
    if (!d)
    {
        print("Empty or not found\n");
    }
    else
    {
        uint32_t count = 0;
        struct dirent* de;
        while ((de = readdir(d)) != NULL)
        {
            if (de->d_type == DT_DIR)
                print("  <DIR>  ");
            else
                print("  <REG>  ");
            print(de->d_name);
            print("\n");
            count++;
        }
        closedir(d);
        print_uint(count);
        print(" entries\n");
    }

    print("\nDone!\n");
    return 0;
}