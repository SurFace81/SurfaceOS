#ifndef LIB_STRING_H
#define LIB_STRING_H

#include "types.h"

int   strlen(const char* s);
int   strcmp(const char* s1, const char* s2);
void* memset(void* dest, int val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);

#endif