#ifndef SFOS_FCNTL_H
#define SFOS_FCNTL_H

#include "abi/types.h"
#include "abi/fcntl.h"

int open(const char* path, int flags, ...);    // mode when O_CREAT
int openat(int dirfd, const char* path, int flags, ...);
// Linux openat2: how->resolve takes RESOLVE_BENEATH (stay below dirfd).
int openat2(int dirfd, const char* path, const struct open_how* how, size_t size);
int creat(const char* path, mode_t mode);
int fcntl(int fd, int cmd, ...);               // F_DUPFD/F_*FD/F_*FL

#endif
