#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITE      (1 << 1)

void initPaging(void*);

#endif