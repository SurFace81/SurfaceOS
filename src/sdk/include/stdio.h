#ifndef SFOS_STDIO_H
#define SFOS_STDIO_H

#include "abi/keyboard.h"
#include "abi/screen.h"

void print(const char* str);
void clear();
void exit(int code);

keyboard_event_t read_key();

void set_cursor(uint32_t col, uint32_t row);
screen_size_t get_screen_size();
void set_color(color_t color);

uint64_t uptime_ms();
void sleep_ms(uint32_t ms);

#endif