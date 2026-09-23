#ifndef SFOS_DIRENT_H
#define SFOS_DIRENT_H

#include "abi/types.h"
#include "abi/dirent.h"     // DT_*

// opendir/readdir/closedir over open(O_DIRECTORY)+getdents64.
// Single-threaded, one buffer per DIR (stage-3 SDK scope).

struct dirent
{
    uint64_t d_ino;
    sint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct DIR DIR;

DIR*           opendir(const char* path);
struct dirent* readdir(DIR* dir);
int            closedir(DIR* dir);

#endif
