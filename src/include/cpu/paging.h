#ifndef PAGING_H
#define PAGING_H

#include "types.h"
#include "memory.h"

#define PAGE_SIZE_BYTES 0x200000

#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_ACCESSED   (1 << 5)
#define PAGE_DIRTY      (1 << 6)
#define PAGE_SIZE       (1 << 7)

#ifdef __cplusplus
extern "C" {
#endif

void initPaging(void*);
void allocate_pages(UINT64 virt, UINT64 phys, UINT64 size);

#ifdef __cplusplus
}
#endif

#endif  // PAGING_H