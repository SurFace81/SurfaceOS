#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_uint(uint32_t n)
{
    char buf[12];
    int pos = 0;
    if (n == 0) { print("0"); return; }
    while (n > 0) { buf[pos++] = '0' + (n % 10); n /= 10; }
    char out[12];
    for (int i = 0; i < pos; i++) out[i] = buf[pos - 1 - i];
    out[pos] = '\0';
    print(out);
}

int main()
{
    print("=== MALLOC TEST ===\n");

    // Allocate many blocks larger than the initial 64 KB grow chunk
    // to force multiple brk() calls. 24 x 8 KB = 192 KB fits the heap.
    const int N = 24;
    const int BLK = 8 * 1024;   // 8 KB each -> 192 KB total
    void* ptrs[N];
    int ok = 0;

    for (int i = 0; i < N; i++)
    {
        ptrs[i] = malloc(BLK);
        if (!ptrs[i]) break;

        // Touch first and last byte to force page mapping
        ((unsigned char*)ptrs[i])[0] = (unsigned char)(i + 1);
        ((unsigned char*)ptrs[i])[BLK - 1] = (unsigned char)(i + 1);
        ok++;
    }

    print("Allocated ");
    print_uint(ok);
    print(" x ");
    print_uint(BLK);
    print(" bytes = ");
    print_uint(ok * BLK);
    print(" bytes\n");

    // Verify the written bytes survived
    int bad = 0;
    for (int i = 0; i < ok; i++)
    {
        if (((unsigned char*)ptrs[i])[0] != (unsigned char)(i + 1)) bad++;
        if (((unsigned char*)ptrs[i])[BLK - 1] != (unsigned char)(i + 1)) bad++;
    }

    print("Verify: ");
    print_uint(bad);
    print(" bad bytes\n");

    // Free everything
    for (int i = 0; i < ok; i++)
        free(ptrs[i]);

    print("Freed all\n");

    // Re-allocate to test reuse of freed blocks
    void* p = malloc(1024);
    print("Realloc 1024: ");
    print(p ? "ok" : "fail");
    print("\n");
    free(p);

    print("Done!\n");
    return 0;
}
