#ifndef ABI_AUXV_H
#define ABI_AUXV_H

#include "types.h"

// ELF auxiliary vector, as Linux x86_64 lays it out above envp on the
// initial process stack. The kernel fills AT_PHDR, AT_PHENT, AT_PHNUM,
// AT_PAGESZ, AT_ENTRY, AT_RANDOM and the AT_NULL terminator.

#define AT_NULL     0       // end of vector
#define AT_IGNORE   1
#define AT_PHDR     3       // program header table in the image
#define AT_PHENT    4       // size of one program header
#define AT_PHNUM    5       // number of program headers
#define AT_PAGESZ   6       // system page size
#define AT_BASE     7       // base address of the interpreter (0: none)
#define AT_ENTRY    9       // entry point of the program
#define AT_RANDOM   25      // address of 16 random bytes

struct auxv_t
{
    uint64_t a_type;
    uint64_t a_val;
};

#endif // ABI_AUXV_H
