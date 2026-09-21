#include "../include/string.h"

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
char* strcpy(char* dest, const char* src)
{
    char* d = dest;
    while ((*d++ = *src++)) {}
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src)
{
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dest;
}

int strncmp(const char* s1, const char* s2, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (s1[i] != s2[i])
            return *(uint8_t*)&s1[i] - *(uint8_t*)&s2[i];
        if (!s1[i])
            return 0;
    }
    return 0;
}

void* memmove(void* dest, const void* src, size_t count)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s)
        for (size_t i = 0; i < count; i++) d[i] = s[i];
    else
        for (size_t i = count; i > 0; i--) d[i - 1] = s[i - 1];
    return dest;
}
