#ifndef SFOS_SYS_STAT_H
#define SFOS_SYS_STAT_H

#include "../abi/types.h"
#include "../abi/stat.h"

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int lstat(const char* path, struct stat* st);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);
int mkdir(const char* path, mode_t mode);

#endif
