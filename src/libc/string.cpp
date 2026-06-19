#include "./include/string.h"

int strlen(const char* s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(uint8_t*)s1 - *(uint8_t*)s2;
}

void* memset(void* dest, int val, size_t count)
{
    uint8_t* d = (uint8_t*)dest;
    for (size_t i = 0; i < count; i++) d[i] = (uint8_t)val;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < count; i++) d[i] = s[i];
    return dest;
}