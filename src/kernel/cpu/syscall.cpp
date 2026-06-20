#include "../../include/cpu/syscall.h"
#include "../../include/drivers/screen.h"
#include "../../include/cpu/program.h"
#include "../../include/drivers/fs/fat32.h"

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

        default:
            regs->rax = (uint64_t)-1;
            break;
    }
}