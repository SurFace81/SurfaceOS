#include "paging.h"

void initPaging(void* TableAddr) {
    UINT64 flags = PAGE_PRESENT | PAGE_WRITE;

    // PML4 table
    UINT64* PML4 = TableAddr;
    *PML4 = ((UINT64)PML4 + 0x1000) | flags;

    // PDP table
    UINT64* PDP = (void*)((UINT64)PML4 + 0x1000);
    *PDP = ((UINT64)PDP + 0x1000) | flags;

    // PD tables (32 items)
    UINT64* PD = (void*)((UINT64)PDP + 0x1000);
    for (UINT64 i = 0; i <= 32; i++) {
        *(PD + i) = (((UINT64)PD + 0x1000) + i * 0x1000) | flags;
    }

    // PT tables (16384 items)
    UINT64* PT = (void*)((UINT64)PD + 0x1000);
    for (UINT64 i = 0; i <= 16384; i++) {
        *(PT + i) = ((UINT64)(0x0000 + i * 0x1000)) | flags;
    }


    // Set new paging
    asm volatile ("mov %0, %%cr3"::"r"(PML4));
}