#ifndef STRING_H
#define STRING_H

#include "../cpu/types.h"

void int_to_str(int num, char str[]);
void hex_to_str(uint64_t num, char str[], uint64_t size);
uint32_t parse_uint(const char* s);

int  strlen(const char* s);
int  strcmp(const char* s1, const char* s2);
int  strncmp(const char* s1, const char* s2, int n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, int n);
void* memset(void* dest, int val, uint64_t count);
void* memcpy(void* dest, const void* src, uint64_t count);

#endif // STRING_H