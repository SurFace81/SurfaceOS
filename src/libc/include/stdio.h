#ifndef LIB_STDIO_H
#define LIB_STDIO_H

#include "keyboard.h"

void print(const char* str);
void exit(int code);
keyboard_event_t read_key();

#endif