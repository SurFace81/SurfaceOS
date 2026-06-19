#ifndef PROGRAM_H
#define PROGRAM_H

#include "types.h"
#include "../drivers/keyboard.h"

#define PROGRAM_BASE    0x3000000
#define PROGRAM_SIZE    (4 * 1024 * 1024)

struct program_info
{
    uint64_t heap_start;
    uint64_t heap_size;
};

namespace program
{
    bool exec(const char* path);
    bool has_key();
    keyboard_event_t pop_key();
}

#endif