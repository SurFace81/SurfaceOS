#include "memory.h"

static UINT64 TOT_MEMORY_SIZE;

void setMemorySize(UINT64 size) {
    TOT_MEMORY_SIZE = size;
}

UINT64 getMemorySize() {
    return TOT_MEMORY_SIZE;
}