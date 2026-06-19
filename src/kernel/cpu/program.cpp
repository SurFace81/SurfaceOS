// program.cpp
#include "../../include/cpu/program.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/cpu/irq.h"
#include "../../include/mm/memory.h"

extern "C" void jump_to_program(uint64_t entry, uint64_t stack, uint64_t arg);

namespace program
{
    static program_info info;

    // Ring buffer for keyboard events while program is running
    static const uint32_t KEY_BUF_SIZE = 64;
    static keyboard_event_t key_buf[KEY_BUF_SIZE];
    static volatile uint32_t key_head = 0;
    static volatile uint32_t key_tail = 0;

    static void program_key_handler(keyboard_event_t e)
    {
        uint32_t next = (key_head + 1) % KEY_BUF_SIZE;
        if (next == key_tail)
            return; // buffer full, drop event

        key_buf[key_head] = e;
        key_head = next;
    }

    bool has_key()
    {
        return key_head != key_tail;
    }

    keyboard_event_t pop_key()
    {
        keyboard_event_t e = key_buf[key_tail];
        key_tail = (key_tail + 1) % KEY_BUF_SIZE;
        return e;
    }

    bool exec(const char* path)
    {
        uint8_t* base = (uint8_t*)PROGRAM_BASE;
        uint32_t max_code = PROGRAM_SIZE / 2;

        memory::memset(base, 0, PROGRAM_SIZE);

        uint32_t bytes_read = fat32::read_file(path, base, max_code);
        if (bytes_read == (uint32_t)-1 || bytes_read == 0)
            return false;

        uint64_t code_end = PROGRAM_BASE + ((bytes_read + 15) & ~15ULL);
        uint64_t stack_top = PROGRAM_BASE + PROGRAM_SIZE;
        uint64_t stack_bottom = stack_top - (64 * 1024);

        info.heap_start = code_end;
        info.heap_size  = stack_bottom - code_end;

        // Switch keyboard input to program buffer
        key_head = 0;
        key_tail = 0;
        keyboard_callback_t prev_callback = keyboard::get_callback();
        keyboard::set_keyboard_callback(program_key_handler);

        irq::pic_send_eoi(1);
        jump_to_program(PROGRAM_BASE, stack_top, (uint64_t)&info);

        // Program finished, restore console keyboard handler
        keyboard::set_keyboard_callback(prev_callback);

        return true;
    }
}