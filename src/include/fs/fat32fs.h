#ifndef FAT32FS_H
#define FAT32FS_H

#include "../cpu/types.h"
#include "../dev/blkdev.h"
#include "vfs.h"

// FAT32 filesystem driver behind vnode_ops (stage 3.3).
//
// Layout of the driver:
//   fat.cpp    cluster chains, the allocator (FSInfo hint), dirty-bit
//   dir.cpp    directory records: 8.3 + LFN read/write, name matching
//   vnode.cpp  vnode_ops, read_at/write_at/truncate/readdir, mount/umount
//
// Every disk access goes through bcache. Identity of a file is the pair
// (directory cluster, short-entry index) - FAT has no inodes - which is
// also what becomes st_ino. The root directory's key is its root_cluster
// with entry index 0xFFFFFFFF.

// On-disk structures -------------------------------------------------------

struct fat_bpb
{
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

struct fat_dir_entry
{
    uint8_t  name[11];            // short 8.3, space padded
    uint8_t  attr;
    uint8_t  nt_flags;            // 0x08: base lower, 0x10: ext lower
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

struct fat_lfn_entry
{
    uint8_t  order;               // sequence | 0x40 on the last (first on disk)
    uint16_t name1[5];
    uint8_t  attr;                // always 0x0F
    uint8_t  type;                // always 0
    uint8_t  checksum;            // of the short name
    uint16_t name2[6];
    uint16_t first_cluster;       // always 0
    uint16_t name3[2];
} __attribute__((packed));

struct fat_fsinfo
{
    uint32_t lead_sig;            // 0x41615252 "RRaA"
    uint8_t  reserved1[480];
    uint32_t struc_sig;           // 0x61417272 "rrAa"
    uint32_t free_count;          // 0xFFFFFFFF: unknown
    uint32_t next_free;           // hint
    uint8_t  reserved2[12];
    uint32_t trail_sig;           // 0xAA550000
} __attribute__((packed));

// Attribute flags
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F    // RO|HID|SYS|VOL together

// Cluster values (28-bit)
#define FAT_CLUSTER_FREE    0x00000000
#define FAT_CLUSTER_BAD     0x0FFFFFF7
#define FAT_CLUSTER_EOC     0x0FFFFFF8   // >= this: end of chain

// Directory markers
#define FAT_DIR_FREE        0xE5
#define FAT_DIR_END         0x00
#define FAT_DIR_LFN_ORD_LAST 0x40

// NT case flags in the short entry
#define FAT_NT_BASE_LOWER   0x08
#define FAT_NT_EXT_LOWER    0x10

// Volume dirty flag: FAT entry 1, bit 27.
#define FAT_VOL_DIRTY       0x08000000

// ---------------------------------------------------------------------------

struct fat_super
{
    blkdev*  dev;
    mount*   mnt;               // the VFS mount this volume is attached to
    bool     active;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;            // sectors per FAT copy
    uint32_t fat_start;           // LBA of FAT copy 0
    uint32_t data_start;          // LBA of cluster 2
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t total_clusters;      // data clusters
    uint32_t fsinfo_sector;
    bool     fsinfo_valid;
    uint32_t free_count;          // FSInfo mirror (0xFFFFFFFF: unknown)
    uint32_t next_free;           // allocator hint

    uint8_t* scratch;             // one cluster-sized RMW/zero-fill buffer

    vnode*   root_vn;             // cached root vnode (ref held by super)
};

// Per-vnode FAT state (vnode::fs_priv).
struct fat_node
{
    fat_super* sb;

    uint32_t first_cluster;       // 0 for an empty file (assigned on write)
    uint32_t parent_cluster;      // directory holding this entry (root: itself)
    uint32_t entry_index;         // physical 32-byte slot of the short entry
                                  // (0xFFFFFFFF for the root)

    // Chain position cache: reading cluster #i after #i-1 is O(1).
    uint32_t last_index;          // which cluster `last_cluster` is (#0 = first)
    uint32_t last_cluster;

    // Where the short entry lives on disk, to update size/time without a
    // full directory rescan. entry_lba is the sector (device-relative),
    // entry_off the byte offset of the 32-byte record inside it.
    uint64_t entry_lba;
    uint32_t entry_off;
};

// fat.cpp ---------------------------------------------------------------------

namespace fat
{
    sint64_t read_entry(fat_super* sb, uint32_t cluster, uint32_t* out);
    sint64_t write_entry(fat_super* sb, uint32_t cluster, uint32_t value);

    // Walk `steps` links forward; used by uncached jumps. `cluster` is
    // updated in place. -EIO on a chain error.
    sint64_t chain_advance(fat_super* sb, uint32_t* cluster, uint32_t steps);

    // Cluster index -> number: follow the chain `index` links from the
    // start, using and updating the vnode's position cache.
    sint64_t cluster_at(fat_node* fn, uint32_t index, uint32_t* out);

    sint64_t alloc_cluster(fat_super* sb, uint32_t* out_cluster);
    sint64_t free_chain(fat_super* sb, uint32_t first_cluster);

    // Append `newc` to the chain of `fn`, extending it by one cluster.
    sint64_t chain_append(fat_node* fn, uint32_t newc);

    // Data I/O of one cluster through bcache (buffer: cluster_size bytes).
    sint64_t read_cluster(fat_super* sb, uint32_t cluster, void* buf);
    sint64_t write_cluster(fat_super* sb, uint32_t cluster, const void* buf);

    // Byte-range I/O across clusters. `v` is the file vnode (its size is
    // updated and it is marked dirty); fn is v->fs_priv. A write past EOF
    // extends the chain and zeroes the hole. Short at EOF via *done.
    sint64_t read_at(vnode* v, fat_node* fn, uint64_t off, void* buf,
                     uint64_t len, uint64_t* done);
    sint64_t write_at(vnode* v, fat_node* fn, uint64_t off, const void* buf,
                      uint64_t len, uint64_t* done);

    sint64_t truncate(vnode* v, fat_node* fn, uint64_t size);

    // FSInfo flush; sets/clears the volume dirty bit in FAT[1].
    sint64_t flush_fsinfo(fat_super* sb);
    sint64_t load_fsinfo(fat_super* sb);   // mount-time; rebuilds the hint
    void     set_dirty_bit(fat_super* sb, bool dirty);
    uint32_t cluster_lba(fat_super* sb, uint32_t cluster);
}

// dir.cpp ---------------------------------------------------------------------

// A directory record: the short entry plus its decoded long name (UTF-8).
struct fat_dirent
{
    fat_dir_entry e;
    char     lfn[NAME_MAX + 1];   // empty when the record has no LFN
    bool     has_lfn;
};

namespace fatdir
{
    // Directory iteration. All functions take the directory's fat_node
    // (its first_cluster + chain position cache).
    //
    // Find `name` (case-insensitive; matches the LFN or the short name).
    // *out_index receives the short-entry slot for the vnode key, and
    // *out_entry_lba/*out_entry_off the on-disk location of the 32-byte
    // record (for the fat_node position cache).
    sint64_t find(fat_node* dir, const char* name,
                  fat_dirent* out, uint32_t* out_index, bool* found,
                  uint64_t* out_entry_lba, uint32_t* out_entry_off);

    // `cookie` is a physical 32-byte slot number; pass the previous
    // *out_next_cookie to resume, 0 to start. Returns the next usable
    // record (LFN slots, free slots and the volume label are skipped) with
    // its slot in *out_index, or *eof=true at the end.
    sint64_t next_record(fat_node* dir, uint64_t cookie,
                         fat_dirent* out, uint32_t* out_index,
                         uint64_t* out_next_cookie, bool* eof);

    // Create a file/directory entry. `is_dir` sets the DIRECTORY attr;
    // `first_cluster` may be 0 (empty file). Returns the short-entry slot
    // and the record location, as find() does. -EEXIST when the name is
    // taken, -EINVAL for an illegal name.
    sint64_t create(fat_node* dir, const char* name,
                    bool is_dir, uint32_t first_cluster,
                    uint32_t* out_index,
                    uint64_t* out_entry_lba, uint32_t* out_entry_off);

    // Write vnode state (size, mtime, first_cluster) back into its short
    // entry on disk (uses the cached entry_lba/entry_off: O(1)).
    sint64_t update_entry(vnode* v, fat_node* fn);

    // Mark the record at slot `index` and its LFN run free (0xE5).
    sint64_t remove(fat_node* dir, uint32_t index);

    // Name helpers (exported for diagnostics/tests).
    bool     valid_name(const char* name);        // charset + length checks
    void     make_short_name(const char* name, uint8_t* out11, bool* needs_lfn,
                             uint8_t* nt_flags);
    uint8_t  short_checksum(const uint8_t* name11);
    uint32_t ucs2_to_utf8_name(const uint16_t* ucs2, uint32_t count,
                               char* out, uint32_t outmax);

    // Physical location of slot `index` in directory `dir`.
    sint64_t slot_location(fat_node* dir, uint32_t index,
                           uint64_t* lba, uint32_t* byte_off);

    // 11-byte short name -> display name (NT flags applied), 13-byte out.
    void     format_short_name(const fat_dir_entry* e, char* out);

    // FAT <-> epoch time (UTC).
    void     epoch_to_fat(uint64_t epoch, uint16_t* date, uint16_t* time);
    uint64_t fat_to_epoch(uint16_t date, uint16_t time);
}

// vnode.cpp ---------------------------------------------------------------------

namespace fat32fs
{
    extern vfs_fs fs;            // registered with vfs::mount_at
}

#endif // FAT32FS_H
