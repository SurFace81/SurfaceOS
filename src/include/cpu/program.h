#ifndef PROGRAM_H
#define PROGRAM_H

#include "types.h"
#include "../drivers/keyboard.h"
#include "../../sdk/include/abi/program.h"

#define PROGRAM_BASE    0x3000000
#define PROGRAM_SIZE    (4 * 1024 * 1024)

namespace program
{
    bool exec(const char* path, int argc = 0, const char** argv = nullptr);
    bool has_key();
    keyboard_event_t pop_key();
}

#endif