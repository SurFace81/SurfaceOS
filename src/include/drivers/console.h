#ifndef CONSOLE_H
#define CONSOLE_H

#include "../cpu/cpuid.h"
#include "../cpu/pci.h"
#include "../mm/memory.h"
#include "../stdlib/list.h"
#include "keyboard.h"
#include "screen.h"

// Command handler: receives raw input buffer, writes output via screen::write/printf
typedef void (*command_fn)(list::List<char>* args);

namespace console
{
    void init();
    void register_command(const char* name, command_fn handler);
} // namespace console

#endif