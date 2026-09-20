#ifndef SFOS_ERRNO_H
#define SFOS_ERRNO_H

#include "abi/errno.h"

// Set by every libc wrapper when it returns an error indicator.
// Single-threaded processes only (no errno-per-thread until musl arrives).
extern int errno;

#endif
