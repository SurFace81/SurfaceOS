// Syscall dispatch (int 0x80).
//
// Rule for this file: a register that holds a user pointer is never
// dereferenced. Everything crosses the boundary through uaccess, which
// checks the address range and the page permissions first. Sizes are capped
// so a hostile argument cannot make the kernel allocate or loop without
// bound. Failures return (uint64_t)-1 and leave the app running.

#include "../../include/cpu/syscall.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/process.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/rtc.h"
#include "../../include/mm/heap.h"
#include "../../sdk/include/abi/fs.h"
#include "../../sdk/include/abi/time.h"

// syscall_entry (interrupts.asm) pushes the GPRs right below the CPU's iret
// frame, so the two structs are contiguous on the kernel stack.
typedef user_regs syscall_regs;

static const uint64_t SYSCALL_ERROR   = (uint64_t)-1;

// Caps on user-controlled sizes.
static const uint64_t MAX_WRITE_BYTES = 64 * 1024;      // one SYS_WRITE call
static const uint64_t MAX_FILE_BYTES  = 16 * 1024 * 1024;
static const uint32_t MAX_DIR_ENTRIES = 256;

// Copy a path argument into kernel memory. False if it is unreadable,
// too long, or empty.
static bool fetch_path(uint64_t user_ptr, char* out)
{
    sint64_t r = uaccess::strncpy_from_user(out, user_ptr, uaccess::MAX_PATH);
    return r > 0;
}

extern "C" void syscall_dispatch(syscall_regs* regs)
{
    iret_frame* iret = (iret_frame*)(regs + 1);

    switch (regs->rax)
    {
        // Process control, keyboard and memory live in process.cpp: they may
        // block or switch to another process by rewriting the trap frame.
        case SYS_EXIT:      process::sys_exit(regs, iret);      break;
        case SYS_READ_KEY:  process::sys_read_key(regs, iret);  break;
        case SYS_READ_LINE: process::sys_read_line(regs, iret); break;
        case SYS_BRK:       process::sys_brk(regs, iret);       break;
        case SYS_GETPID:    process::sys_getpid(regs, iret);    break;
        case SYS_GETPPID:   process::sys_getppid(regs, iret);   break;
        case SYS_FORK:      process::sys_fork(regs, iret);      break;
        case SYS_EXEC:      process::sys_exec(regs, iret);      break;
        case SYS_WAITPID:   process::sys_waitpid(regs, iret);   break;
        case SYS_YIELD:     process::sys_yield(regs, iret);     break;
        case SYS_SLEEP:     process::sys_sleep(regs, iret);     break;
        case SYS_KILL:      process::sys_kill(regs, iret);      break;
        case SYS_MMAP:      process::sys_mmap(regs, iret);      break;
        case SYS_MUNMAP:    process::sys_munmap(regs, iret);    break;
        case SYS_MPROTECT:  process::sys_mprotect(regs, iret);  break;

        case SYS_WRITE:
        {
            // Pull the string across in page-sized bites so a long line still
            // prints, while a string with no terminator stops at the cap.
            char buf[257];
            uint64_t ptr = regs->rdi;
            uint64_t written = 0;
            regs->rax = 0;

            while (written < MAX_WRITE_BYTES)
            {
                sint64_t r = uaccess::strncpy_from_user(buf, ptr, sizeof(buf));
                if (r == -1)
                {
                    regs->rax = SYSCALL_ERROR;
                    break;
                }

                screen::printf("%s", buf);
                uart::printf("%s", buf);   // app output on the serial line too

                if (r >= 0)             // reached the terminator
                    break;

                ptr     += sizeof(buf) - 1;
                written += sizeof(buf) - 1;
            }
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
            char path[uaccess::MAX_PATH];
            uint64_t size = regs->rdx;

            if (!fetch_path(regs->rdi, path) || size > MAX_FILE_BYTES ||
                !uaccess::readable(regs->rsi, size))
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            // Bounce through kernel memory: the FAT32 driver must never see
            // a user pointer, and under SMAP it could not read one anyway.
            uint8_t* buf = (uint8_t*)kmalloc(size ? size : 1);
            if (!buf)
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            if (!uaccess::copy_from_user(buf, regs->rsi, size))
            {
                kfree(buf);
                regs->rax = SYSCALL_ERROR;
                break;
            }

            regs->rax = (uint64_t)fat32::write_file(path, buf, (uint32_t)size);
            kfree(buf);
            break;
        }

        case SYS_READ_FILE:
        {
            char path[uaccess::MAX_PATH];
            uint64_t max_size = regs->rdx;

            if (!fetch_path(regs->rdi, path) || max_size == 0 ||
                max_size > MAX_FILE_BYTES ||
                !uaccess::writable(regs->rsi, max_size))
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            uint8_t* buf = (uint8_t*)kmalloc(max_size);
            if (!buf)
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            uint32_t read = fat32::read_file(path, buf, (uint32_t)max_size);
            if (read == (uint32_t)-1)
            {
                kfree(buf);
                regs->rax = SYSCALL_ERROR;
                break;
            }

            regs->rax = uaccess::copy_to_user(regs->rsi, buf, read)
                      ? (uint64_t)read : SYSCALL_ERROR;
            kfree(buf);
            break;
        }

        case SYS_STAT_FILE:
        {
            char path[uaccess::MAX_PATH];

            if (!fetch_path(regs->rdi, path) ||
                !uaccess::writable(regs->rsi, sizeof(file_stat_t)))
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            fat32_dir_entry entry;
            if (!fat32::resolve_path_pub(path, &entry))
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            file_stat_t out;
            out.size        = entry.file_size;
            out.attr        = entry.attr;
            out.create_date = entry.create_date;
            out.create_time = entry.create_time;
            out.modify_date = entry.write_date;
            out.modify_time = entry.write_time;

            regs->rax = uaccess::copy_to_user(regs->rsi, &out, sizeof(out))
                      ? 0 : SYSCALL_ERROR;
            break;
        }

        case SYS_READ_DIR:
        {
            char path[uaccess::MAX_PATH];
            uint64_t max_entries = regs->rdx;

            if (max_entries == 0 || max_entries > MAX_DIR_ENTRIES ||
                !uaccess::writable(regs->rsi, max_entries * sizeof(dir_entry_t)))
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            // An empty path means "current directory"; anything else must be
            // a readable string.
            if (regs->rdi == 0)
                path[0] = '\0';
            else if (uaccess::strncpy_from_user(path, regs->rdi, sizeof(path)) < 0)
            {
                regs->rax = SYSCALL_ERROR;
                break;
            }

            const uint32_t batch = 32;
            fat32_dir_entry raw[batch];
            dir_entry_t out[batch];

            uint32_t want = (uint32_t)max_entries;
            if (want > batch)
                want = batch;

            uint32_t count = fat32::ls(path, raw, want);

            for (uint32_t i = 0; i < count; i++)
            {
                format_83_name(raw[i].name, out[i].name);
                out[i].size        = raw[i].file_size;
                out[i].attr        = raw[i].attr;
                out[i].modify_date = raw[i].write_date;
                out[i].modify_time = raw[i].write_time;
            }

            regs->rax = uaccess::copy_to_user(regs->rsi, out, count * sizeof(dir_entry_t))
                      ? (uint64_t)count : SYSCALL_ERROR;
            break;
        }

        case SYS_UPTIME:
        {
            uint64_t ms = pit::uptime_ms();
            uint64_t total_sec = ms / 1000;

            uptime_t out;
            out.total_ms = ms;
            out.seconds  = (uint32_t)(total_sec % 60);
            out.minutes  = (uint32_t)((total_sec / 60) % 60);
            out.hours    = (uint32_t)(total_sec / 3600);

            regs->rax = uaccess::copy_to_user(regs->rdi, &out, sizeof(out))
                      ? 0 : SYSCALL_ERROR;
            break;
        }

        case SYS_TIME:
        {
            rtc_time t;
            rtc::read(&t);

            datetime_t out;
            out.year    = t.year;
            out.month   = t.month;
            out.day     = t.day;
            out.hours   = t.hours;
            out.minutes = t.minutes;
            out.seconds = t.seconds;
            out.weekday = t.weekday;

            regs->rax = uaccess::copy_to_user(regs->rdi, &out, sizeof(out))
                      ? 0 : SYSCALL_ERROR;
            break;
        }

        default:
            regs->rax = SYSCALL_ERROR;
            break;
    }

    process::syscall_return(regs, iret);
}
