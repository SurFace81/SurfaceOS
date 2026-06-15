#include "../../include/stdlib/string.h"

static const char digits[] = "0123456789ABCDEF";

static void reverse(char* s, int len)
{
    for (int i = 0, j = len - 1; i < j; i++, j--)
    {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
}

void int_to_str(int num, char str[])
{
    if (num < 0)
    {
        str[0] = '-';
        // avoid overflow on INT_MIN: cast after negation digit-by-digit
        uint32_t u = (uint32_t)(-(num + 1)) + 1;
        int i = 1;

        do
        {
            str[i++] = digits[u % 10];
            u /= 10;
        } while (u);

        str[i] = '\0';
        reverse(str + 1, i - 1);
        return;
    }

    uint32_t u = (uint32_t)num;
    int i = 0;

    do
    {
        str[i++] = digits[u % 10];
        u /= 10;
    } while (u);

    str[i] = '\0';
    reverse(str, i);
}

void hex_to_str(uint64_t num, char str[], uint64_t size)
{
    for (uint64_t i = 0; i < size; i++)
        str[i] = '0';
    str[size] = '\0';

    uint64_t i = size;
    while (num && i > 0)
    {
        i--;
        str[i] = digits[num & 0xF];
        num >>= 4;
    }
}

int strlen(const char* s)
{
    int i = 0;
    while (s[i]) i++;

    return i;
}

int strcmp(const char* s1, const char* s2)
{
    while (*s1 && *s1 == *s2)
    {
        s1++;
        s2++;
    }

    return (uint8_t)*s1 - (uint8_t)*s2;
}

int strncmp(const char* s1, const char* s2, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (s1[i] != s2[i] || s1[i] == '\0' || s2[i] == '\0')
        {
            return (uint8_t)s1[i] - (uint8_t)s2[i];
        }            
    }

    return 0;
}

char* strcpy(char* dest, const char* src)
{
    char* start = dest;
    while ((*dest++ = *src++));

    return start;
}

char* strncpy(char* dest, const char* src, int n)
{
    char* start = dest;
    int i = 0;

    while (i < n && src[i])
    {
        dest[i] = src[i];
        i++;
    }

    // Pad remaining space with null bytes
    while (i < n)
    {
        dest[i] = '\0';
        i++;
    }

    return start;
}

void* memset(void* dest, int val, uint64_t count)
{
    uint8_t* p = (uint8_t*)dest;
    for (uint64_t i = 0; i < count; i++)
    {
        p[i] = (uint8_t)val;
    }

    return dest;
}

void* memcpy(void* dest, const void* src, uint64_t count)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < count; i++)
    {
        d[i] = s[i];
    }

    return dest;
}