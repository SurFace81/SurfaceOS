#ifndef SFOS_MMAN_H
#define SFOS_MMAN_H

#include "abi/types.h"
#include "abi/process.h"

// Anonymous, private, zero-filled memory. `addr` is a hint (page-aligned,
// honoured when that range is free). Returns MAP_FAILED on error.
void* mmap(void* addr, size_t length, int prot);
int   munmap(void* addr, size_t length);

// Change protection of pages in the image, heap, mmap region or stack.
// PROT_WRITE|PROT_EXEC together is allowed (JIT / tcc -run).
int   mprotect(void* addr, size_t length, int prot);

// Raw program break: brk(0) returns the current break.
uint64_t brk(uint64_t new_brk);

#endif
