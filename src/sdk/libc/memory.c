// libc: memcpy, memmove, memset, memcmp. The compiler calls them on its
// own too, for struct copies and zeroing.

#include <string.h>

void* memcpy(void* Dest, const void* Src, size_t Size)
{
    unsigned char*       D = (unsigned char*)Dest;
    const unsigned char* S = (const unsigned char*)Src;
    while (Size--)
        *D++ = *S++;
    return Dest;
}

void* memmove(void* Dest, const void* Src, size_t Size)
{
    unsigned char*       D = (unsigned char*)Dest;
    const unsigned char* S = (const unsigned char*)Src;
    if (D <= S || D >= S + Size)
        return memcpy(Dest, Src, Size);
    while (Size--)                      // from the end: Dest overlaps Src's tail
        D[Size] = S[Size];
    return Dest;
}

void* memset(void* Dest, int Value, size_t Size)
{
    unsigned char* D = (unsigned char*)Dest;
    while (Size--)
        *D++ = (unsigned char)Value;
    return Dest;
}

int memcmp(const void* A, const void* B, size_t Size)
{
    const unsigned char* X = (const unsigned char*)A;
    const unsigned char* Y = (const unsigned char*)B;
    for (; Size; Size--, X++, Y++)
        if (*X != *Y)
            return *X - *Y;
    return 0;
}
