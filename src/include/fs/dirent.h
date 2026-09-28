#ifndef FS_DIRENT_H
#define FS_DIRENT_H

#include "../cpu/types.h"

// One directory entry as vnode_ops readdir fills it. d_name is padded so the
// next record starts on an 8-byte boundary; d_reclen is the record size
// including padding.
struct dir_record
{
    uint64_t d_ino;
    sint64_t d_off;       // opaque cookie: position after this entry
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];    // NUL-terminated, then padding to d_reclen
};

// d_type values
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

#endif // FS_DIRENT_H
