#ifndef STDIO_H
#define STDIO_H

#include "string.h"
#include "../cpu/types.h"
#include "../drivers/screen.h"
#include "list.h"

void print(const char*);
void print(list::List<char>* char_list);
void print(int);
void print(char);
void print(uint64_t, uint64_t size);

#endif  // STDIO_H