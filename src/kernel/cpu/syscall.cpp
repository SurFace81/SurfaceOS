#include "../../include/cpu/syscall.h"
#include "../../include/drivers/screen.h"
#include "../../include/cpu/program.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/rtc.h"
#include "../../sdk/include/abi/fs.h"
#include "../../sdk/include/abi/time.h"

extern "C" void return_to_kernel();

struct syscall_regs
{
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp;
    uint64_t rdi, rsi, rdx, rcx, rbx, rax;
};

extern "C" void syscall_dispatch(syscall_regs* regs)
{
    switch (regs->rax)
    {
        case SYS_EXIT:
        {
            return_to_kernel();
            break;
        }

        case SYS_WRITE:
        {
            screen::printf("%s", (const char*)regs->rdi);
            regs->rax = 0;
            break;
        }

        case SYS_READ_KEY:
        {
            // Block until a key event arrives
            while (!program::has_key())
                asm volatile("sti; hlt");

            keyboard_event_t e = program::pop_key();

            // Return event via pointer passed in rdi
            keyboard_event_t* out = (keyboard_event_t*)regs->rdi;
            *out = e;
            regs->rax = 0;
            break;
        }

        case SYS_READ_LINE:
        {
            char* buf = (char*)regs->rdi;
            uint32_t max_len = (uint32_t)regs->rsi;

            if (!buf || max_len == 0)
            {
                regs->rax = 0;
                break;
            }

            uint32_t pos = 0;
            uint32_t limit = max_len - 1; // reserve space for null terminator

            while (pos < limit)
            {
                while (!program::has_key())
                    asm volatile("sti; hlt");

                keyboard_event_t e = program::pop_key();

                if (e.type != KEY_PRESS)
                    continue;

                if (e.KeyCode == KEY_ENTER)
                {
                    screen::printf("\n");
                    break;
                }

                if (e.KeyCode == KEY_BACKSPACE)
                {
                    if (pos > 0)
                    {
                        pos--;
                        screen::printf("\b \b");    // screen::backspace();
                    }
                    continue;
                }

                if (e.KeyChar)
                {
                    buf[pos++] = e.KeyChar;
                    char tmp[2] = { e.KeyChar, 0 };
                    screen::printf("%s", tmp);
                }
            }

            buf[pos] = '\0';
            regs->rax = (uint64_t)pos;
            break;
        }

        case SYS_CLEAR:
        {
            screen::clear();
            regs->rax = 0;
            break;
        }

        case SYS_SET_CURSOR:
        {
            screen::set_cursor((uint32_t)regs->rdi, (uint32_t)regs->rsi);
            regs->rax = 0;
            break;
        }

        case SYS_WRITE_FILE:
        {
            const char* path = (const char*)regs->rdi;
            const uint8_t* data = (const uint8_t*)regs->rsi;
            uint32_t size = (uint32_t)regs->rdx;
            regs->rax = (uint64_t)fat32::write_file(path, data, size);
            break;
        }

        case SYS_READ_FILE:
        {
            const char* path = (const char*)regs->rdi;
            uint8_t* buffer = (uint8_t*)regs->rsi;
            uint32_t max_size = (uint32_t)regs->rdx;
            regs->rax = (uint64_t)fat32::read_file(path, buffer, max_size);
            break;
        }

        case SYS_STAT_FILE:
        {
            const char* path = (const char*)regs->rdi;
            file_stat_t* out = (file_stat_t*)regs->rsi;

            if (!path || !out)
            {
                regs->rax = (uint64_t)-1;
                break;
            }

            fat32_dir_entry entry;
            if (!fat32::resolve_path_pub(path, &entry))
            {
                regs->rax = (uint64_t)-1;
                break;
            }

            out->size        = entry.file_size;
            out->attr        = entry.attr;
            out->create_date = entry.create_date;
            out->create_time = entry.create_time;
            out->modify_date = entry.write_date;
            out->modify_time = entry.write_time;
            regs->rax = 0;
            break;
        }

        case SYS_READ_DIR:
        {
            const char* path = (const char*)regs->rdi;
            dir_entry_t* out = (dir_entry_t*)regs->rsi;
            uint32_t max_entries = (uint32_t)regs->rdx;

            if (!out || max_entries == 0)
            {
                regs->rax = 0;
                break;
            }

            const uint32_t tmp_max = 64;
            fat32_dir_entry raw[tmp_max];
            uint32_t count = fat32::ls(path ? path : "", raw,
                                       max_entries < tmp_max ? max_entries : tmp_max);

            for (uint32_t i = 0; i < count && i < max_entries; i++)
            {
                format_83_name(raw[i].name, out[i].name);
                out[i].size        = raw[i].file_size;
                out[i].attr        = raw[i].attr;
                out[i].modify_date = raw[i].write_date;
                out[i].modify_time = raw[i].write_time;
            }

            regs->rax = (uint64_t)count;
            break;
        }

        case SYS_UPTIME:
        {
            uptime_t* out = (uptime_t*)regs->rdi;
            if (!out)
            {
                regs->rax = (uint64_t)-1;
                break;
            }

            uint64_t ms = pit::uptime_ms();
            uint64_t total_sec = ms / 1000;

            out->total_ms = ms;
            out->seconds  = (uint32_t)(total_sec % 60);
            out->minutes  = (uint32_t)((total_sec / 60) % 60);
            out->hours    = (uint32_t)(total_sec / 3600);
            regs->rax = 0;
            break;
        }

        case SYS_TIME:
        {
            datetime_t* out = (datetime_t*)regs->rdi;
            if (!out)
            {
                regs->rax = (uint64_t)-1;
                break;
            }

            rtc_time t;
            rtc::read(&t);

            out->year    = t.year;
            out->month   = t.month;
            out->day     = t.day;
            out->hours   = t.hours;
            out->minutes = t.minutes;
            out->seconds = t.seconds;
            out->weekday = t.weekday;
            regs->rax = 0;
            break;
        }

        default:
            regs->rax = (uint64_t)-1;
            break;
    }
}