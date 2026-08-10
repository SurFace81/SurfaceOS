#ifndef ABI_PROGRAM_H
#define ABI_PROGRAM_H

#include "types.h"

#define PROGRAM_MAX_ARGS   16
#define PROGRAM_ARG_MAX    64

// Shared between kernel loader and SDK entry point, so both sides
// always agree on the layout passed into a program
struct program_info
{
    uint64_t heap_start;
    uint64_t heap_size;

    int  argc;
    char argv[PROGRAM_MAX_ARGS][PROGRAM_ARG_MAX];
};

#endif  // ABI_PROGRAM_H