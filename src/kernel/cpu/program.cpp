#include "../../include/cpu/program.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/screen.h"
#include "../../include/mm/memory.h"

// Defined in interrupts.asm
extern "C" void jump_to_program(uint64_t entry, uint64_t stack, uint64_t arg);

namespace program
{
    static program_info info;

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

        jump_to_program(PROGRAM_BASE, stack_top, (uint64_t)&info);

        return true;
    }
}