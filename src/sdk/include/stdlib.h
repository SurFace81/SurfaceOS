#ifndef SFOS_STDLIB_H
#define SFOS_STDLIB_H

#include "abi/types.h"

// The process environment (envp from the initial stack). NULL if none.
extern char** environ;

const char* getenv(const char* name);

void* malloc(size_t size);
void  free(void* ptr);

#endif
