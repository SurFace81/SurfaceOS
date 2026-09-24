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

    // Run any command queued by the keyboard handler. Called by the console
    // task (main), never from an interrupt: commands like `exec` do heavy
    // synchronous work (USB I/O, page mapping) and must not run inside the
    // keyboard IRQ.
    void poll();

    // The console task: sleeps until a command line is entered, runs it,
    // forever. Started by kmain through process::start_console.
    void main(void*);
} // namespace console

#endif