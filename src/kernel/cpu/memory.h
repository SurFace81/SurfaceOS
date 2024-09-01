#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void setMemorySize(UINT64);
UINT64 getMemorySize();

#ifdef __cplusplus
}
#endif

#endif