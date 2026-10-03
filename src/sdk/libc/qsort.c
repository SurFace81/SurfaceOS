// libc: qsort, as a Shell sort - no recursion, no memory of its own.

#include <stdlib.h>

static void Swap(unsigned char* A, unsigned char* B, size_t Size)
{
    while (Size--)
    {
        unsigned char T = *A;
        *A++ = *B;
        *B++ = T;
    }
}

void qsort(void* Base, size_t Count, size_t Size, int (*Compare)(const void* A, const void* B))
{
    unsigned char* Items = (unsigned char*)Base;
    size_t Gap = 1;
    while (Gap < Count / 3)
        Gap = Gap * 3 + 1;              // 1, 4, 13, 40, ...
    for (; Gap; Gap /= 3)
        for (size_t i = Gap; i < Count; i++)
            for (size_t j = i; j >= Gap &&
                 Compare(Items + (j - Gap) * Size, Items + j * Size) > 0; j -= Gap)
                Swap(Items + (j - Gap) * Size, Items + j * Size, Size);
}
