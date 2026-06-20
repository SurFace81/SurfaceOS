#ifndef SFOS_STDIO_H
#define SFOS_STDIO_H

#include "abi/keyboard.h"
#include "abi/screen.h"

void print(const char* str);
void clear();
void set_cursor(uint32_t col, uint32_t row);
void exit(int code);

keyboard_event_t read_key();
uint32_t read_line(char* buffer, uint32_t max_len);

uint32_t write_file(const char* path, const uint8_t* data, uint32_t size);
uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size);

#endif