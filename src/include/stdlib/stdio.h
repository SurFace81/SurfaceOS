#ifndef STDIO_H
#define STDIO_H

#include "string.h"
#include "../cpu/types.h"
#include "../drivers/screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void print_str(const char*);
void print_dec(int);
void print_hex(UINT64, UINT64 size);

#ifdef __cplusplus
}
#endif

#endif  // STDIO_H