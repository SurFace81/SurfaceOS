#ifndef SFOS_STDIO_H
#define SFOS_STDIO_H

#include "abi/keyboard.h"
#include "abi/screen.h"

void print(const char* str);
void clear();
void set_cursor(uint32_t col, uint32_t row);
void exit(int code);

keyboard_event_t read_key();

#endif