// src/kernel/drivers/fs/fat32.cpp
#include "../../../include/drivers/fs/fat32.h"
#include "../../../include/drivers/usb/xhci.h"
#include "../../../include/drivers/uart.h"
#include "../../../include/drivers/screen.h"
#include "../../../include/mm/memory.h"
#include "../../../include/mm/heap.h"
#include "../../../include/stdlib/string.h"

// Cached volume parameters after mount
static bool mounted = false;
static uint8_t dev_index = 0;
static uint32_t bytes_per_sector = 0;
static uint8_t  sectors_per_cluster = 0;
static uint32_t fat_start_lba = 0;       // LBA of first FAT
static uint32_t data_start_lba = 0;      // LBA of cluster 2
static uint32_t root_cluster = 0;
static uint32_t fat_size_sectors = 0;
static uint8_t  num_fats = 0;

// Convert cluster number to LBA
static uint32_t cluster_to_lba(uint32_t cluster)
{
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

// Read one sector from the USB device
static bool read_sector(uint32_t lba, void* buffer)
{
    return usb::read_sectors(dev_index, lba, 1, buffer) == USB_OK;
}

// Read the FAT entry for a given cluster, returns next cluster in chain
static uint32_t fat_next_cluster(uint32_t cluster)
{
    // Each FAT entry is 4 bytes. Figure out which sector of the FAT
    // contains this entry, and the offset within that sector.
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / bytes_per_sector);
    uint32_t entry_offset = fat_offset % bytes_per_sector;

    uint8_t* buf = (uint8_t*)kmalloc(bytes_per_sector);
    if (!buf) return FAT32_CLUSTER_BAD;

    if (!read_sector(fat_sector, buf))
    {
        kfree(buf);
        return FAT32_CLUSTER_BAD;
    }

    uint32_t val = *(uint32_t*)(buf + entry_offset);
    val &= 0x0FFFFFFF; // FAT32 uses 28 bits

    kfree(buf);
    return val;
}

// Read entire cluster into buffer. Returns bytes read (cluster size).
static uint32_t read_cluster(uint32_t cluster, void* buffer)
{
    uint32_t lba = cluster_to_lba(cluster);
    uint32_t size = (uint32_t)sectors_per_cluster * bytes_per_sector;

    if (usb::read_sectors(dev_index, lba, sectors_per_cluster, buffer) != USB_OK)
        return 0;

    return size;
}

// Compare two strings, case-insensitive for ASCII letters
static bool str_eq_nocase(const char* a, const char* b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return true;
}

// Convert a FAT 8.3 dir entry name to a readable "NAME.EXT" string.
// Returns length written (no null terminator added by this function).
static uint32_t format_83_name(const uint8_t* raw, char* out)
{
    uint32_t pos = 0;

    // Base name (first 8 bytes), trim trailing spaces
    uint32_t base_len = 8;
    while (base_len > 0 && raw[base_len - 1] == ' ')
        base_len--;

    for (uint32_t i = 0; i < base_len; i++)
        out[pos++] = raw[i];

    // Extension (bytes 8..10), trim trailing spaces
    uint32_t ext_len = 3;
    while (ext_len > 0 && raw[8 + ext_len - 1] == ' ')
        ext_len--;

    if (ext_len > 0)
    {
        out[pos++] = '.';
        for (uint32_t i = 0; i < ext_len; i++)
            out[pos++] = raw[8 + i];
    }

    out[pos] = '\0';
    return pos;
}

// Convert user-provided filename to 8.3 format for comparison.
// Fills 11 bytes in out, space-padded. Returns false if name is invalid.
static bool to_83_name(const char* name, uint8_t* out)
{
    memory::memset(out, ' ', 11);

    uint32_t len = strlen(name);
    if (len == 0 || len > 12) return false;

    // Find the dot
    int dot_pos = -1;
    for (uint32_t i = 0; i < len; i++)
    {
        if (name[i] == '.')
        {
            dot_pos = (int)i;
            break;
        }
    }

    // Base name
    uint32_t base_len = (dot_pos >= 0) ? (uint32_t)dot_pos : len;
    if (base_len > 8) return false;

    for (uint32_t i = 0; i < base_len; i++)
    {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }

    // Extension
    if (dot_pos >= 0)
    {
        uint32_t ext_start = dot_pos + 1;
        uint32_t ext_len = len - ext_start;
        if (ext_len > 3) return false;

        for (uint32_t i = 0; i < ext_len; i++)
        {
            char c = name[ext_start + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = c;
        }
    }

    return true;
}

// Find a directory entry by name in a directory starting at the given cluster.
// On success, fills *out_entry and returns true.
static bool find_in_dir(uint32_t dir_cluster, const char* name, fat32_dir_entry* out_entry)
{
    uint8_t search_name[11];
    if (!to_83_name(name, search_name))
        return false;

    uint32_t cluster_size = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint8_t* buf = (uint8_t*)kmalloc(cluster_size);
    if (!buf) return false;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (read_cluster(cluster, buf) == 0)
            break;

        uint32_t entries = cluster_size / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END)
            {
                kfree(buf);
                return false;
            }
            if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE)
                continue;
            if (dir[i].attr == FAT32_ATTR_LFN)
                continue;
            if (dir[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            if (str_eq_nocase((const char*)dir[i].name, (const char*)search_name, 11))
            {
                *out_entry = dir[i];
                kfree(buf);
                return true;
            }
        }

        cluster = fat_next_cluster(cluster);
    }

    kfree(buf);
    return false;
}

// Resolve a path like "DIR1/DIR2/FILE.TXT" starting from root.
// Returns the directory entry of the final component.
static bool resolve_path(const char* path, fat32_dir_entry* out_entry)
{
    if (!path || path[0] == '\0')
        return false;

    // Skip leading '/'
    while (*path == '/') path++;
    if (*path == '\0')
        return false;

    uint32_t current_cluster = root_cluster;

    // Walk each path component
    while (*path)
    {
        // Extract next component
        char component[13];
        uint32_t clen = 0;
        while (*path && *path != '/' && clen < 12)
        {
            component[clen++] = *path++;
        }
        component[clen] = '\0';

        while (*path == '/') path++;

        fat32_dir_entry entry;
        if (!find_in_dir(current_cluster, component, &entry))
            return false;

        if (*path == '\0')
        {
            // This is the final component
            *out_entry = entry;
            return true;
        }

        // Not the last component — must be a directory
        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return false;

        current_cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
    }

    return false;
}

// Public API

namespace fat32
{
    bool mount(uint8_t usb_dev)
    {
        mounted = false;

        uint8_t* sector = (uint8_t*)kmalloc(512);
        if (!sector) return false;

        if (usb::read_sectors(usb_dev, 0, 1, sector) != USB_OK)
        {
            kfree(sector);
            return false;
        }

        fat32_bpb* bpb = (fat32_bpb*)sector;

        // Basic sanity checks
        if (bpb->bytes_per_sector < 512 || bpb->sectors_per_cluster == 0 ||
            bpb->num_fats == 0 || bpb->fat_size_32 == 0)
        {
            uart::printf("fat32: invalid BPB\n");
            kfree(sector);
            return false;
        }

        dev_index = usb_dev;
        bytes_per_sector = bpb->bytes_per_sector;
        sectors_per_cluster = bpb->sectors_per_cluster;
        num_fats = bpb->num_fats;
        fat_size_sectors = bpb->fat_size_32;
        root_cluster = bpb->root_cluster;
        fat_start_lba = bpb->reserved_sectors;
        data_start_lba = bpb->reserved_sectors + (uint32_t)bpb->num_fats * bpb->fat_size_32;

        uart::printf("fat32: mounted OK\n");
        uart::printf("  bytes/sector:    %u\n", bytes_per_sector);
        uart::printf("  sectors/cluster: %u\n", (uint32_t)sectors_per_cluster);
        uart::printf("  FAT start LBA:   %u\n", fat_start_lba);
        uart::printf("  data start LBA:  %u\n", data_start_lba);
        uart::printf("  root cluster:    %u\n", root_cluster);
        uart::printf("  FAT size:        %u sectors (%u KB)\n",
                     fat_size_sectors, fat_size_sectors * bytes_per_sector / 1024);

        mounted = true;
        kfree(sector);
        return true;
    }

    bool ls(const char* path)
    {
        if (!mounted) return false;

        uint32_t dir_cluster = root_cluster;

        // If path is not null/empty/"/" — resolve it to a directory
        if (path && path[0] != '\0' && !(path[0] == '/' && path[1] == '\0'))
        {
            fat32_dir_entry entry;
            if (!resolve_path(path, &entry))
            {
                screen::printf("not found: %s\n\r", path);
                return false;
            }
            if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            {
                screen::printf("not a directory: %s\n\r", path);
                return false;
            }
            dir_cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
        }

        uint32_t cluster_size = (uint32_t)sectors_per_cluster * bytes_per_sector;
        uint8_t* buf = (uint8_t*)kmalloc(cluster_size);
        if (!buf) return false;

        uint32_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
        {
            if (read_cluster(cluster, buf) == 0)
                break;

            uint32_t entries = cluster_size / sizeof(fat32_dir_entry);
            fat32_dir_entry* dir = (fat32_dir_entry*)buf;

            for (uint32_t i = 0; i < entries; i++)
            {
                if (dir[i].name[0] == FAT32_DIR_ENTRY_END)
                {
                    kfree(buf);
                    return true;
                }
                if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE)
                    continue;
                if (dir[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (dir[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;

                char name[13];
                format_83_name(dir[i].name, name);

                if (dir[i].attr & FAT32_ATTR_DIRECTORY)
                    screen::printf("  <DIR>  %s\n\r", name);
                else
                    screen::printf("  %u\t %s\n\r", dir[i].file_size, name);
            }

            cluster = fat_next_cluster(cluster);
        }

        kfree(buf);
        return true;
    }

    uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size)
    {
        if (!mounted || !buffer || max_size == 0)
            return -1;

        fat32_dir_entry entry;
        if (!resolve_path(path, &entry))
            return -1;

        if (entry.attr & FAT32_ATTR_DIRECTORY)
            return -1;

        uint32_t file_size = entry.file_size;
        if (file_size > max_size)
            file_size = max_size;

        uint32_t cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
        uint32_t cluster_size = (uint32_t)sectors_per_cluster * bytes_per_sector;
        uint8_t* cluster_buf = (uint8_t*)kmalloc(cluster_size);
        if (!cluster_buf) return -1;

        uint32_t bytes_read = 0;
        while (cluster >= 2 && cluster < FAT32_CLUSTER_END && bytes_read < file_size)
        {
            if (read_cluster(cluster, cluster_buf) == 0)
                break;

            uint32_t to_copy = cluster_size;
            if (bytes_read + to_copy > file_size)
                to_copy = file_size - bytes_read;

            memory::memcpy(cluster_buf, buffer + bytes_read, to_copy);
            bytes_read += to_copy;

            cluster = fat_next_cluster(cluster);
        }

        kfree(cluster_buf);
        return (uint32_t)bytes_read;
    }
}