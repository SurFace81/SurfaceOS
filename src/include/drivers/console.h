#ifndef CONSOLE_H
#define CONSOLE_H

#include "../cpu/cpuid.h"
#include "../cpu/pci.h"
#include "../mm/memory.h"
#include "../stdlib/list.h"
#include "keyboard.h"
#include "screen.h"

#define CONSOLE_MAX_ARGS 16
#define CONSOLE_INPUT_MAX 256

// Сommand signature: argc/argv like in standard C
typedef void (*command_fn)(int argc, const char** argv);

namespace console
{
    void init();
    void register_command(const char* name, command_fn handler);

    uint32_t command_count();
    const char* command_name(uint32_t index);
} // namespace console

#endif