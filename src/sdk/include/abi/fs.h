#ifndef ABI_FS_H
#define ABI_FS_H

#include "types.h"

// File attributes (subset of FAT32 attrs)
#define FILE_ATTR_READ_ONLY  0x01
#define FILE_ATTR_HIDDEN     0x02
#define FILE_ATTR_SYSTEM     0x04
#define FILE_ATTR_DIRECTORY  0x10
#define FILE_ATTR_ARCHIVE    0x20

struct file_stat_t
{
    uint32_t size;
    uint8_t  attr;
    uint16_t create_date;
    uint16_t create_time;
    uint16_t modify_date;
    uint16_t modify_time;
};

struct dir_entry_t
{
    char     name[13];  // 8.3 formatted + null
    uint32_t size;
    uint8_t  attr;
    uint16_t modify_date;
    uint16_t modify_time;
};

#endif