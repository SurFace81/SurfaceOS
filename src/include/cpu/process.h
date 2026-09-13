#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "paging.h"
#include "../drivers/keyboard.h"

// Flat-binary app memory layout inside the user address space.
// All regions live in PML4[USER_PML4_INDEX], far from kernel mappings.
#define USER_INFO_VADDR     USER_BASE                   // 1 page: program_info
#define USER_IMAGE_VADDR    (USER_BASE + 0x100000)      // app code+data (flat bin)
#define USER_IMAGE_MAX      (4 * 1024 * 1024)
#define USER_REGION_END     (USER_BASE + 0x1400000)     // 20 MB total window
#define USER_STACK_SIZE     (256 * 1024)
#define USER_STACK_TOP      USER_REGION_END             // stack grows down from here

#define KERNEL_STACK_SIZE   (64 * 1024)

struct program_info
{
    uint64_t heap_start;
    uint64_t heap_size;
};

namespace process
{
    // Run a flat-binary app from the filesystem in ring 3 with its own
    // address space. Blocks until the app exits (SYS_EXIT) or faults.
    // Returns true if the app was loaded and ran.
    bool run(const char* path);

    // Called by the syscall dispatcher on SYS_EXIT (does not return,
    // control goes back to run()).
    void exit_current() __attribute__((noreturn));

    // Keyboard event queue for the running app (filled by the keyboard
    // IRQ while the app is active).
    bool has_key();
    keyboard_event_t pop_key();
}

#endif // PROCESS_H
