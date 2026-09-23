#ifndef SFOS_STRING_H
#define SFOS_STRING_H

#include "abi/types.h"

int   strlen(const char* s);
int   strcmp(const char* s1, const char* s2);
int   strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
void* memset(void* dest, int val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);

#endif
