#ifndef SFOS_STDIO_H
#define SFOS_STDIO_H

#include "abi/keyboard.h"
#include "abi/screen.h"
#include "abi/time.h"

// Console output helpers (write(1, ...) under the hood).
void print(const char* str);
void print_u64(uint64_t value);
void print_i64(sint64_t value);
void print_hex64(uint64_t value);   // 0x-prefixed
void clear();
void set_cursor(uint32_t col, uint32_t row);
void exit(int code);

keyboard_event_t read_key();

// Line input from fd 0 (canonical tty read). Returns the length without
// the trailing '\n' (NUL is appended), or 0 at EOF.
uint32_t read_line(char* buffer, uint32_t max_len);

void     get_uptime(uptime_t* out);
void     get_time(datetime_t* out);

#endif
