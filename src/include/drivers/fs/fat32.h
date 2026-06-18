// src/include/drivers/fs/fat32.h
#ifndef FAT32_H
#define FAT32_H

#include "../../cpu/types.h"

// BPB + extended BPB for FAT32 (first sector of the volume)
struct fat32_bpb {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;         // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed));

// Standard 8.3 directory entry
struct fat32_dir_entry {
    uint8_t  name[11];            // 8.3 format, space-padded
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed));

// Directory entry attribute flags
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F

// Special cluster values
#define FAT32_CLUSTER_FREE    0x00000000
#define FAT32_CLUSTER_BAD     0x0FFFFFF7
#define FAT32_CLUSTER_END     0x0FFFFFF8  // >= this means end of chain

// End-of-directory markers
#define FAT32_DIR_ENTRY_FREE  0xE5
#define FAT32_DIR_ENTRY_END   0x00

uint32_t format_83_name(const uint8_t* raw, char* out);
void format_datetime(uint16_t date, uint16_t time, char* out);

namespace fat32 {
    bool     mount(uint8_t usb_dev_index);
    void     umount();
    bool     is_mounted();
    uint32_t ls(const char* path, fat32_dir_entry* entries, uint32_t max_entries);
    uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size);
    uint32_t write_file(const char* path, const uint8_t* data, uint32_t size);
    bool     mkdir(const char* path);
    bool     remove(const char* path);
    bool     rename(const char* old_path, const char* new_name);
    bool     copy(const char* src_path, const char* dst_path);

    bool        set_cwd(const char* path);
    const char* cwd_path();
}

#endif