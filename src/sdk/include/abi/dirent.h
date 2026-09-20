#ifndef ABI_DIRENT_H
#define ABI_DIRENT_H

#include "types.h"

// struct linux_dirent64, exactly as getdents64 fills it. d_name is padded so
// the next record starts on an 8-byte boundary; d_reclen is the record size
// including padding.

struct linux_dirent64
{
    uint64_t d_ino;
    sint64_t d_off;       // opaque cookie: position after this entry
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];    // NUL-terminated, then padding to d_reclen
};

// d_type values (same as dirent.h on Linux)
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

#endif // ABI_DIRENT_H
