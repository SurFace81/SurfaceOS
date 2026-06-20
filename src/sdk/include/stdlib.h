#ifndef SFOS_STDLIB_H
#define SFOS_STDLIB_H

#include "abi/types.h"

void  heap_init(void* start, size_t size);
void* malloc(size_t size);
void  free(void* ptr);

#endif