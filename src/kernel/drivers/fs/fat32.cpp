// src/kernel/drivers/fs/fat32.cpp
#include "../../../include/drivers/fs/fat32.h"
#include "../../../include/drivers/usb/xhci.h"
#include "../../../include/drivers/uart.h"
#include "../../../include/drivers/screen.h"
#include "../../../include/mm/memory.h"
#include "../../../include/mm/heap.h"
#include "../../../include/stdlib/string.h"

#define CWD_PATH_MAX 256
#define PATH_SEPARATOR '\\'

static uint32_t cwd_cluster = 0;
static char     cwd_path_buf[CWD_PATH_MAX] = "\\";

// Cached volume parameters after mount
static bool mounted = false;
static uint8_t dev_index = 0;
static uint32_t bytes_per_sector = 0;
static uint8_t  sectors_per_cluster = 0;
static uint32_t fat_start_lba = 0;
static uint32_t data_start_lba = 0;
static uint32_t root_cluster = 0;
static uint32_t fat_size_sectors = 0;
static uint8_t  num_fats = 0;
static uint32_t total_sectors = 0;

// Helpers

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

static uint32_t cluster_size()
{
    return (uint32_t)sectors_per_cluster * bytes_per_sector;
}

static bool read_sector(uint32_t lba, void* buffer)
{
    return usb::read_sectors(dev_index, lba, 1, buffer) == USB_OK;
}

static bool write_sector(uint32_t lba, const void* buffer)
{
    return usb::write_sectors(dev_index, lba, 1, buffer) == USB_OK;
}

static bool read_cluster_data(uint32_t cluster, void* buffer)
{
    uint32_t lba = cluster_to_lba(cluster);
    return usb::read_sectors(dev_index, lba, sectors_per_cluster, buffer) == USB_OK;
}

static bool write_cluster_data(uint32_t cluster, const void* buffer)
{
    uint32_t lba = cluster_to_lba(cluster);
    return usb::write_sectors(dev_index, lba, sectors_per_cluster, buffer) == USB_OK;
}

// Build a clean absolute path from current cwd + relative component
// Handles "..", ".", leading/trailing slashes
static bool normalize_path(const char* base, const char* rel, char* out, uint32_t max)
{
    // Start with base path split into components
    // We use a simple stack of component start-offsets

    char temp[CWD_PATH_MAX];
    uint32_t temp_len = 0;

    // Copy base into temp (skip leading /)
    const char* bp = base;
    while (*bp == PATH_SEPARATOR) bp++;
    while (*bp && temp_len < CWD_PATH_MAX - 2)
    {
        temp[temp_len++] = *bp++;
    }

    // Append separator if base is non-empty
    if (temp_len > 0 && temp[temp_len - 1] != PATH_SEPARATOR)
        temp[temp_len++] = PATH_SEPARATOR;

    // Append relative path
    const char* rp = rel;
    while (*rp == PATH_SEPARATOR) rp++;
    while (*rp && temp_len < CWD_PATH_MAX - 2)
    {
        temp[temp_len++] = *rp++;
    }
    temp[temp_len] = '\0';

    // Now process components, resolving . and ..
    // Component pointers stored as a simple stack
    const char* comps[32];
    uint32_t comp_count = 0;

    char* p = temp;
    while (*p)
    {
        // Skip slashes
        while (*p == PATH_SEPARATOR) p++;
        if (*p == '\0') break;

        char* start = p;
        while (*p && *p != PATH_SEPARATOR) p++;

        uint32_t len = (uint32_t)(p - start);

        if (len == 1 && start[0] == '.')
            continue;

        if (len == 2 && start[0] == '.' && start[1] == '.')
        {
            if (comp_count > 0)
                comp_count--;
            continue;
        }

        // Null-terminate this component in place
        if (*p) { *p = '\0'; p++; }

        if (comp_count < 32)
            comps[comp_count++] = start;
    }

    // Build output
    out[0] = PATH_SEPARATOR;
    uint32_t pos = 1;

    for (uint32_t i = 0; i < comp_count; i++)
    {
        uint32_t clen = strlen(comps[i]);
        if (pos + clen + 1 >= max)
            return false;

        memcpy(out + pos, comps[i], clen);
        pos += clen;

        if (i + 1 < comp_count)
            out[pos++] = PATH_SEPARATOR;
    }

    out[pos] = '\0';
    return true;
}

// FAT date: bits 15-9 = year-1980, bits 8-5 = month, bits 4-0 = day
// FAT time: bits 15-11 = hours, bits 10-5 = minutes, bits 4-0 = seconds/2
void format_datetime(uint16_t date, uint16_t time, char* out)
{
    if (date == 0 && time == 0)
    {
        strcpy(out, "                ");
        return;
    }

    uint16_t day   = date & 0x1F;
    uint16_t month = (date >> 5) & 0x0F;
    uint16_t year  = ((date >> 9) & 0x7F) + 1980;
    uint16_t hour  = (time >> 11) & 0x1F;
    uint16_t min   = (time >> 5) & 0x3F;

    // Format: DD.MM.YYYY HH:MM
    out[0]  = '0' + day / 10;
    out[1]  = '0' + day % 10;
    out[2]  = '.';
    out[3]  = '0' + month / 10;
    out[4]  = '0' + month % 10;
    out[5]  = '.';
    out[6]  = '0' + (year / 1000) % 10;
    out[7]  = '0' + (year / 100) % 10;
    out[8]  = '0' + (year / 10) % 10;
    out[9]  = '0' + year % 10;
    out[10] = ' ';
    out[11] = '0' + hour / 10;
    out[12] = '0' + hour % 10;
    out[13] = ':';
    out[14] = '0' + min / 10;
    out[15] = '0' + min % 10;
    out[16] = '\0';
}

// FAT table operations

static uint32_t fat_read_entry(uint32_t cluster)
{
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
    val &= 0x0FFFFFFF;

    kfree(buf);
    return val;
}

// Write a FAT entry to all copies of the FAT
static bool fat_write_entry(uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_in_fat = fat_offset / bytes_per_sector;
    uint32_t entry_offset = fat_offset % bytes_per_sector;

    uint8_t* buf = (uint8_t*)kmalloc(bytes_per_sector);
    if (!buf) return false;

    for (uint8_t f = 0; f < num_fats; f++)
    {
        uint32_t fat_sector = fat_start_lba + f * fat_size_sectors + sector_in_fat;

        if (!read_sector(fat_sector, buf))
        {
            kfree(buf);
            return false;
        }

        // Preserve upper 4 bits
        uint32_t* entry = (uint32_t*)(buf + entry_offset);
        *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

        if (!write_sector(fat_sector, buf))
        {
            kfree(buf);
            return false;
        }
    }

    kfree(buf);
    return true;
}

// Allocate a free cluster, mark as end-of-chain, zero it out.
// Returns cluster number or 0 on failure.
static uint32_t fat_alloc_cluster()
{
    uint32_t total_clusters = (total_sectors - data_start_lba) / sectors_per_cluster;
    uint32_t max_cluster = total_clusters + 1;

    for (uint32_t c = 2; c <= max_cluster; c++)
    {
        uint32_t val = fat_read_entry(c);
        if (val == FAT32_CLUSTER_FREE)
        {
            if (!fat_write_entry(c, 0x0FFFFFFF))
                return 0;

            uint32_t csize = cluster_size();
            uint8_t* zero = (uint8_t*)kmalloc(csize);
            if (zero)
            {
                memory::memset(zero, 0, csize);
                write_cluster_data(c, zero);
                kfree(zero);
            }

            return c;
        }
    }

    return 0;
}

// Free entire cluster chain
static void fat_free_chain(uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
}

// String helpers

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

uint32_t format_83_name(const uint8_t* raw, char* out)
{
    uint32_t pos = 0;

    uint32_t base_len = 8;
    while (base_len > 0 && raw[base_len - 1] == ' ')
        base_len--;

    for (uint32_t i = 0; i < base_len; i++)
        out[pos++] = raw[i];

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

static bool to_83_name(const char* name, uint8_t* out)
{
    memory::memset(out, ' ', 11);

    uint32_t len = strlen(name);
    if (len == 0 || len > 12) return false;

    int dot_pos = -1;
    for (uint32_t i = 0; i < len; i++)
    {
        if (name[i] == '.')
        {
            dot_pos = (int)i;
            break;
        }
    }

    uint32_t base_len = (dot_pos >= 0) ? (uint32_t)dot_pos : len;
    if (base_len > 8) return false;

    for (uint32_t i = 0; i < base_len; i++)
    {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }

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

// Directory operations

static bool find_in_dir(uint32_t dir_cluster, const char* name, fat32_dir_entry* out_entry)
{
    uint8_t search_name[11];
    if (!to_83_name(name, search_name))
        return false;

    uint32_t csize = cluster_size();
    uint8_t* buf = (uint8_t*)kmalloc(csize);
    if (!buf) return false;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (!read_cluster_data(cluster, buf))
            break;

        uint32_t entries = csize / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END)
            {
                kfree(buf);
                return false;
            }
            if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE) continue;
            if (dir[i].attr == FAT32_ATTR_LFN) continue;
            if (dir[i].attr & FAT32_ATTR_VOLUME_ID) continue;

            if (str_eq_nocase((const char*)dir[i].name, (const char*)search_name, 11))
            {
                *out_entry = dir[i];
                kfree(buf);
                return true;
            }
        }

        cluster = fat_read_entry(cluster);
    }

    kfree(buf);
    return false;
}

static uint32_t get_entry_cluster(const fat32_dir_entry* entry)
{
    return ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
}

static void set_entry_cluster(fat32_dir_entry* entry, uint32_t cluster)
{
    entry->first_cluster_hi = (uint16_t)(cluster >> 16);
    entry->first_cluster_lo = (uint16_t)(cluster & 0xFFFF);
}

// Resolve path, also returns parent directory cluster if needed
static bool resolve_path(const char* path, fat32_dir_entry* out_entry, uint32_t* out_parent_cluster = nullptr)
{
    if (!path || path[0] == '\0')
        return false;

    // If relative path, build absolute first
    char abs_buf[CWD_PATH_MAX];
    const char* resolved = path;

    if (path[0] != PATH_SEPARATOR)
    {
        if (!normalize_path(cwd_path_buf, path, abs_buf, CWD_PATH_MAX))
            return false;
        resolved = abs_buf;
    }

    const char* p = resolved;
    while (*p == PATH_SEPARATOR) p++;
    if (*p == '\0')
        return false;

    uint32_t current_cluster = root_cluster;

    while (*p)
    {
        char component[13];
        uint32_t clen = 0;
        while (*p && *p != PATH_SEPARATOR && clen < 12)
            component[clen++] = *p++;
        component[clen] = '\0';

        while (*p == PATH_SEPARATOR) p++;

        fat32_dir_entry entry;
        if (!find_in_dir(current_cluster, component, &entry))
            return false;

        if (*p == '\0')
        {
            if (out_parent_cluster)
                *out_parent_cluster = current_cluster;
            *out_entry = entry;
            return true;
        }

        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return false;

        current_cluster = get_entry_cluster(&entry);
    }

    return false;
}

// Split "dir1/dir2/file.txt" into parent="dir1/dir2" and name="file.txt"
static bool split_path(const char* path, char* parent_out, uint32_t parent_max,
                        char* name_out)
{
    while (*path == PATH_SEPARATOR) path++;
    if (*path == '\0') return false;

    uint32_t len = strlen(path);

    int last_slash = -1;
    for (uint32_t i = 0; i < len; i++)
    {
        if (path[i] == PATH_SEPARATOR)
            last_slash = (int)i;
    }

    if (last_slash < 0)
    {
        parent_out[0] = '\0';
        strncpy(name_out, path, 12);
        name_out[12] = '\0';
    }
    else
    {
        uint32_t plen = (uint32_t)last_slash;
        if (plen >= parent_max) plen = parent_max - 1;
        strncpy(parent_out, path, plen);
        parent_out[plen] = '\0';

        const char* name_start = path + last_slash + 1;
        strncpy(name_out, name_start, 12);
        name_out[12] = '\0';
    }

    return name_out[0] != '\0';
}

// Get starting cluster of a directory by path. 0 = error (except root).
static uint32_t resolve_dir_cluster(const char* path)
{
    if (!path || path[0] == '\0')
        return cwd_cluster;

    const char* p = path;
    while (*p == PATH_SEPARATOR) p++;
    if (*p == '\0')
        return root_cluster;

    // Absolute path
    if (path[0] == PATH_SEPARATOR)
    {
        fat32_dir_entry entry;
        if (!resolve_path(path, &entry))
            return 0;
        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return 0;
        return get_entry_cluster(&entry);
    }

    // Relative path - resolve from cwd
    char abs_path[CWD_PATH_MAX];
    if (!normalize_path(cwd_path_buf, path, abs_path, CWD_PATH_MAX))
        return 0;

    // Root case after normalization
    if (abs_path[0] == PATH_SEPARATOR && abs_path[1] == '\0')
        return root_cluster;

    fat32_dir_entry entry;
    if (!resolve_path(abs_path, &entry))
        return 0;
    if (!(entry.attr & FAT32_ATTR_DIRECTORY))
        return 0;
    return get_entry_cluster(&entry);
}

// Add a new entry to directory. Finds free slot or extends with new cluster.
static bool add_dir_entry(uint32_t dir_cluster, const fat32_dir_entry* entry)
{
    uint32_t csize = cluster_size();
    uint8_t* buf = (uint8_t*)kmalloc(csize);
    if (!buf) return false;

    uint32_t cluster = dir_cluster;
    uint32_t prev_cluster = 0;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (!read_cluster_data(cluster, buf))
        {
            kfree(buf);
            return false;
        }

        uint32_t entries = csize / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END ||
                dir[i].name[0] == FAT32_DIR_ENTRY_FREE)
            {
                dir[i] = *entry;

                if (!write_cluster_data(cluster, buf))
                {
                    kfree(buf);
                    return false;
                }
                kfree(buf);
                return true;
            }
        }

        prev_cluster = cluster;
        cluster = fat_read_entry(cluster);
    }

    // No free slot, allocate new cluster for directory
    uint32_t new_cluster = fat_alloc_cluster();
    if (new_cluster == 0)
    {
        kfree(buf);
        return false;
    }

    fat_write_entry(prev_cluster, new_cluster);

    memory::memset(buf, 0, csize);
    fat32_dir_entry* dir = (fat32_dir_entry*)buf;
    dir[0] = *entry;

    if (!write_cluster_data(new_cluster, buf))
    {
        kfree(buf);
        return false;
    }

    kfree(buf);
    return true;
}

// Update existing entry matched by 8.3 name
static bool update_dir_entry(uint32_t dir_cluster, const uint8_t* name83,
                              const fat32_dir_entry* new_entry)
{
    uint32_t csize = cluster_size();
    uint8_t* buf = (uint8_t*)kmalloc(csize);
    if (!buf) return false;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (!read_cluster_data(cluster, buf))
            break;

        uint32_t entries = csize / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END) break;
            if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE) continue;
            if (dir[i].attr == FAT32_ATTR_LFN) continue;

            if (str_eq_nocase((const char*)dir[i].name, (const char*)name83, 11))
            {
                dir[i] = *new_entry;
                write_cluster_data(cluster, buf);
                kfree(buf);
                return true;
            }
        }

        cluster = fat_read_entry(cluster);
    }

    kfree(buf);
    return false;
}

// Mark entry as deleted
static bool delete_dir_entry(uint32_t dir_cluster, const uint8_t* name83)
{
    uint32_t csize = cluster_size();
    uint8_t* buf = (uint8_t*)kmalloc(csize);
    if (!buf) return false;

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (!read_cluster_data(cluster, buf))
            break;

        uint32_t entries = csize / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END) break;
            if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE) continue;
            if (dir[i].attr == FAT32_ATTR_LFN) continue;

            if (str_eq_nocase((const char*)dir[i].name, (const char*)name83, 11))
            {
                dir[i].name[0] = FAT32_DIR_ENTRY_FREE;
                write_cluster_data(cluster, buf);
                kfree(buf);
                return true;
            }
        }

        cluster = fat_read_entry(cluster);
    }

    kfree(buf);
    return false;
}

// Check if directory has only . and .. entries
static bool is_dir_empty(uint32_t dir_cluster)
{
    uint32_t csize = cluster_size();
    uint8_t* buf = (uint8_t*)kmalloc(csize);
    if (!buf) return false;

    bool empty = true;
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
    {
        if (!read_cluster_data(cluster, buf))
            break;

        uint32_t entries = csize / sizeof(fat32_dir_entry);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        for (uint32_t i = 0; i < entries; i++)
        {
            if (dir[i].name[0] == FAT32_DIR_ENTRY_END) break;
            if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE) continue;
            if (dir[i].attr == FAT32_ATTR_LFN) continue;
            if (dir[i].attr & FAT32_ATTR_VOLUME_ID) continue;

            if (dir[i].name[0] == '.' &&
                (dir[i].name[1] == ' ' ||
                 (dir[i].name[1] == '.' && dir[i].name[2] == ' ')))
                continue;

            empty = false;
            break;
        }

        if (!empty) break;
        cluster = fat_read_entry(cluster);
    }

    kfree(buf);
    return empty;
}

// Public API

namespace fat32
{
    void umount()
    {
        mounted = false;
        cwd_cluster = 0;
        cwd_path_buf[0] = PATH_SEPARATOR;
        cwd_path_buf[1] = '\0';
        bytes_per_sector = 0;
        sectors_per_cluster = 0;
    }

    bool is_mounted()
    {
        return mounted;
    }

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
        total_sectors = bpb->total_sectors_32;

        mounted = true;

        cwd_cluster = root_cluster;
        cwd_path_buf[0] = PATH_SEPARATOR;
        cwd_path_buf[1] = '\0';

        kfree(sector);
        return true;
    }

    uint32_t ls(const char* path, fat32_dir_entry* entries, uint32_t max_entries)
    {
        if (!mounted) return 0;

        uint32_t dir_cluster = cwd_cluster;

        if (path && path[0] == PATH_SEPARATOR && path[1] == '\0')
        {
            dir_cluster = root_cluster;
        }
        else if (path && path[0] != '\0')
        {
            fat32_dir_entry entry;
            if (!resolve_path(path, &entry))
                return 0;
            if (!(entry.attr & FAT32_ATTR_DIRECTORY))
                return 0;
            dir_cluster = get_entry_cluster(&entry);
        }

        uint32_t csize = cluster_size();
        uint8_t* buf = (uint8_t*)kmalloc(csize);
        if (!buf) return 0;

        uint32_t count = 0;
        uint32_t cluster = dir_cluster;

        while (cluster >= 2 && cluster < FAT32_CLUSTER_END)
        {
            if (!read_cluster_data(cluster, buf))
                break;

            uint32_t entries_per = csize / sizeof(fat32_dir_entry);
            fat32_dir_entry* dir = (fat32_dir_entry*)buf;

            for (uint32_t i = 0; i < entries_per; i++)
            {
                if (dir[i].name[0] == FAT32_DIR_ENTRY_END)
                {
                    kfree(buf);
                    return count;
                }
                if (dir[i].name[0] == FAT32_DIR_ENTRY_FREE) continue;
                if (dir[i].attr == FAT32_ATTR_LFN) continue;
                if (dir[i].attr & FAT32_ATTR_VOLUME_ID) continue;

                if (count < max_entries)
                    entries[count] = dir[i];
                count++;
            }

            cluster = fat_read_entry(cluster);
        }

        kfree(buf);
        return count;
    }

    uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size)
    {
        if (!mounted || !buffer || max_size == 0)
            return (uint32_t)-1;

        fat32_dir_entry entry;
        if (!resolve_path(path, &entry))
            return (uint32_t)-1;

        if (entry.attr & FAT32_ATTR_DIRECTORY)
            return (uint32_t)-1;

        uint32_t file_size = entry.file_size;
        if (file_size > max_size)
            file_size = max_size;

        uint32_t cluster = get_entry_cluster(&entry);
        uint32_t csize = cluster_size();
        uint8_t* cluster_buf = (uint8_t*)kmalloc(csize);
        if (!cluster_buf) return (uint32_t)-1;

        uint32_t bytes_read = 0;
        while (cluster >= 2 && cluster < FAT32_CLUSTER_END && bytes_read < file_size)
        {
            if (!read_cluster_data(cluster, cluster_buf))
                break;

            uint32_t to_copy = csize;
            if (bytes_read + to_copy > file_size)
                to_copy = file_size - bytes_read;

            memory::memcpy(buffer + bytes_read, cluster_buf, to_copy);
            bytes_read += to_copy;

            cluster = fat_read_entry(cluster);
        }

        kfree(cluster_buf);
        return bytes_read;
    }

    uint32_t write_file(const char* path, const uint8_t* data, uint32_t size)
    {
        if (!mounted)
            return (uint32_t)-1;

        char parent_path[256];
        char file_name[13];
        if (!split_path(path, parent_path, 256, file_name))
            return (uint32_t)-1;

        uint8_t name83[11];
        if (!to_83_name(file_name, name83))
            return (uint32_t)-1;

        uint32_t parent_cluster = resolve_dir_cluster(parent_path);
        if (parent_cluster == 0 && parent_path[0] != '\0')
            return (uint32_t)-1;

        // Check if file already exists
        fat32_dir_entry existing;
        bool exists = find_in_dir(parent_cluster, file_name, &existing);

        if (exists && (existing.attr & FAT32_ATTR_DIRECTORY))
            return (uint32_t)-1;

        // Free old cluster chain if overwriting
        if (exists)
        {
            uint32_t old_cluster = get_entry_cluster(&existing);
            if (old_cluster >= 2)
                fat_free_chain(old_cluster);
        }

        // Allocate clusters for new data
        uint32_t csize = cluster_size();
        uint32_t clusters_needed = (size > 0) ? (size + csize - 1) / csize : 0;

        uint32_t first_cluster = 0;
        uint32_t prev_cluster = 0;

        for (uint32_t i = 0; i < clusters_needed; i++)
        {
            uint32_t c = fat_alloc_cluster();
            if (c == 0)
            {
                if (first_cluster)
                    fat_free_chain(first_cluster);
                return (uint32_t)-1;
            }

            if (i == 0)
                first_cluster = c;
            else
                fat_write_entry(prev_cluster, c);

            prev_cluster = c;
        }

        // Write data to clusters
        uint32_t bytes_written = 0;
        uint32_t cluster = first_cluster;
        uint8_t* cluster_buf = nullptr;

        if (clusters_needed > 0)
        {
            cluster_buf = (uint8_t*)kmalloc(csize);
            if (!cluster_buf)
            {
                if (first_cluster)
                    fat_free_chain(first_cluster);
                return (uint32_t)-1;
            }
        }

        while (cluster >= 2 && cluster < FAT32_CLUSTER_END && bytes_written < size)
        {
            uint32_t to_write = csize;
            if (bytes_written + to_write > size)
                to_write = size - bytes_written;

            memory::memset(cluster_buf, 0, csize);
            memory::memcpy(cluster_buf, (uint8_t*)data + bytes_written, to_write);

            if (!write_cluster_data(cluster, cluster_buf))
                break;

            bytes_written += to_write;
            cluster = fat_read_entry(cluster);
        }

        if (cluster_buf)
            kfree(cluster_buf);

        // Create or update directory entry
        fat32_dir_entry new_entry;
        memory::memset((uint8_t*)&new_entry, 0, sizeof(fat32_dir_entry));
        memory::memcpy(new_entry.name, name83, 11);
        new_entry.attr = FAT32_ATTR_ARCHIVE;
        set_entry_cluster(&new_entry, first_cluster);
        new_entry.file_size = size;

        if (exists)
            update_dir_entry(parent_cluster, name83, &new_entry);
        else
            add_dir_entry(parent_cluster, &new_entry);

        return bytes_written;
    }

    bool mkdir(const char* path)
    {
        if (!mounted) return false;

        char parent_path[256];
        char dir_name[13];
        if (!split_path(path, parent_path, 256, dir_name))
            return false;

        uint8_t name83[11];
        if (!to_83_name(dir_name, name83))
            return false;

        uint32_t parent_cluster = resolve_dir_cluster(parent_path);
        if (parent_cluster == 0 && parent_path[0] != '\0')
            return false;

        // Check if already exists
        fat32_dir_entry existing;
        if (find_in_dir(parent_cluster, dir_name, &existing))
            return false;

        // Allocate cluster for new directory
        uint32_t new_cluster = fat_alloc_cluster();
        if (new_cluster == 0) return false;

        // Initialize with . and .. entries
        uint32_t csize = cluster_size();
        uint8_t* buf = (uint8_t*)kmalloc(csize);
        if (!buf)
        {
            fat_free_chain(new_cluster);
            return false;
        }

        memory::memset(buf, 0, csize);
        fat32_dir_entry* dir = (fat32_dir_entry*)buf;

        // "." entry
        memory::memset((uint8_t*)dir[0].name, ' ', 11);
        dir[0].name[0] = '.';
        dir[0].attr = FAT32_ATTR_DIRECTORY;
        set_entry_cluster(&dir[0], new_cluster);

        // ".." entry
        memory::memset((uint8_t*)dir[1].name, ' ', 11);
        dir[1].name[0] = '.';
        dir[1].name[1] = '.';
        dir[1].attr = FAT32_ATTR_DIRECTORY;
        set_entry_cluster(&dir[1], parent_cluster);

        if (!write_cluster_data(new_cluster, buf))
        {
            kfree(buf);
            fat_free_chain(new_cluster);
            return false;
        }
        kfree(buf);

        // Add entry to parent
        fat32_dir_entry new_entry;
        memory::memset((uint8_t*)&new_entry, 0, sizeof(fat32_dir_entry));
        memory::memcpy(new_entry.name, name83, 11);
        new_entry.attr = FAT32_ATTR_DIRECTORY;
        set_entry_cluster(&new_entry, new_cluster);
        new_entry.file_size = 0;

        return add_dir_entry(parent_cluster, &new_entry);
    }

    bool remove(const char* path)
    {
        if (!mounted) return false;

        uint32_t parent_cluster;
        fat32_dir_entry entry;
        if (!resolve_path(path, &entry, &parent_cluster))
            return false;

        uint32_t entry_cluster = get_entry_cluster(&entry);

        if (entry.attr & FAT32_ATTR_DIRECTORY)
        {
            if (!is_dir_empty(entry_cluster))
                return false;
        }

        if (entry_cluster >= 2)
            fat_free_chain(entry_cluster);

        uint8_t name83[11];
        memory::memcpy(name83, entry.name, 11);

        return delete_dir_entry(parent_cluster, name83);
    }

    bool rename(const char* old_path, const char* new_name)
    {
        if (!mounted) return false;

        // new_name must be a plain filename, not a path
        for (uint32_t i = 0; new_name[i]; i++)
        {
            if (new_name[i] == PATH_SEPARATOR)
                return false;
        }

        uint8_t new83[11];
        if (!to_83_name(new_name, new83))
            return false;

        uint32_t parent_cluster;
        fat32_dir_entry entry;
        if (!resolve_path(old_path, &entry, &parent_cluster))
            return false;

        // Check that new name doesn't already exist in the same directory
        fat32_dir_entry conflict;
        if (find_in_dir(parent_cluster, new_name, &conflict))
            return false;

        // Build the updated entry with the new name
        uint8_t old83[11];
        memory::memcpy(old83, entry.name, 11);

        fat32_dir_entry updated = entry;
        memory::memcpy(updated.name, new83, 11);

        return update_dir_entry(parent_cluster, old83, &updated);
    }

    bool copy(const char* src_path, const char* dst_path)
    {
        if (!mounted) return false;

        fat32_dir_entry src_entry;
        if (!resolve_path(src_path, &src_entry))
            return false;

        // Cannot copy directories
        if (src_entry.attr & FAT32_ATTR_DIRECTORY)
            return false;

        uint32_t file_size = src_entry.file_size;

        // Zero-length file: just create an empty entry
        if (file_size == 0)
            return write_file(dst_path, nullptr, 0) != (uint32_t)-1;

        // Read source data
        uint8_t* data = (uint8_t*)kmalloc(file_size);
        if (!data) return false;

        uint32_t bytes_read = read_file(src_path, data, file_size);
        if (bytes_read == (uint32_t)-1 || bytes_read == 0)
        {
            kfree(data);
            return false;
        }

        // Write to destination (will overwrite if exists)
        uint32_t written = write_file(dst_path, data, bytes_read);
        kfree(data);

        return written != (uint32_t)-1;
    }

    bool set_cwd(const char* path)
    {
        if (!mounted) return false;

        // "/" — go to root
        if (path[0] == PATH_SEPARATOR && path[1] == '\0')
        {
            cwd_cluster = root_cluster;
            cwd_path_buf[0] = PATH_SEPARATOR;
            cwd_path_buf[1] = '\0';
            return true;
        }

        // Build absolute path
        char new_path[CWD_PATH_MAX];
        if (path[0] == PATH_SEPARATOR)
        {
            // Absolute
            if (!normalize_path("\\", path, new_path, CWD_PATH_MAX))
                return false;
        }
        else
        {
            // Relative to cwd
            if (!normalize_path(cwd_path_buf, path, new_path, CWD_PATH_MAX))
                return false;
        }

        // Root after normalization (e.g. "cd .." from top-level dir)
        if (new_path[0] == PATH_SEPARATOR && new_path[1] == '\0')
        {
            cwd_cluster = root_cluster;
            cwd_path_buf[0] = PATH_SEPARATOR;
            cwd_path_buf[1] = '\0';
            return true;
        }

        // Verify it exists and is a directory
        fat32_dir_entry entry;
        if (!resolve_path(new_path, &entry))
            return false;

        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return false;

        cwd_cluster = get_entry_cluster(&entry);
        strcpy(cwd_path_buf, new_path);
        return true;
    }

    const char* cwd_path()
    {
        return cwd_path_buf;
    }
}