#ifndef VFS_H
#define VFS_H

#include "../cpu/types.h"
#include "../dev/blkdev.h"
#include "../../sdk/include/abi/stat.h"

// Virtual filesystem layer (stage 3.3/3.4).
//
//   syscalls / console
//        |
//      namei  (path -> vnode, handles . .. mounts // trailing /)
//        |
//      vnode cache (two opens of one file share one vnode)
//        |
//    vnode_ops  <- fat32, devfs
//
// Every operation returns 0/>=0 on success or a negative errno. vnodes are
// reference counted; a caller that gets a vnode* owns one reference and must
// vfs::unref() it. The cache holds one reference of its own, so a vnode
// stays alive (and keeps its size/chain cache) while it is merely cached.
//
// Paths are '/'-separated UTF-8 everywhere; a component is at most NAME_MAX
// bytes and a whole path at most PATH_MAX. There are no symlinks: readlink
// is -EINVAL and lstat == stat.

#define PATH_MAX    4096
#define NAME_MAX    255

enum class vtype : uint8_t
{
    REG,        // regular file
    DIR,        // directory
    CHR,        // character device (devfs)
};

struct vnode;
struct mount;
struct vfs_fs;

// One readdir step. The FS decodes its own records (LFN assembly for FAT)
// and reports the plain result plus the cookie to resume from.
struct dirent_out
{
    uint64_t ino;
    uint64_t next_cookie;   // opaque position after this entry
    uint8_t  type;          // DT_* from abi/dirent.h
    char     name[NAME_MAX + 1];
};

struct vnode_ops
{
    // --- namespace ---
    sint64_t (*lookup)(vnode* dir, const char* name, vnode** out);
    // Create a regular file; `mode` already had umask applied. Returns a
    // referenced vnode in *out.
    sint64_t (*create)(vnode* dir, const char* name, uint32_t mode, vnode** out);
    sint64_t (*mkdir)(vnode* dir, const char* name, uint32_t mode);
    sint64_t (*unlink)(vnode* dir, const char* name);
    sint64_t (*rmdir)(vnode* dir, const char* name);
    // flags: 0 or RENAME_NOREPLACE / RENAME_EXCHANGE (abi/fcntl.h).
    sint64_t (*rename)(vnode* old_dir, const char* old_name,
                       vnode* new_dir, const char* new_name, uint32_t flags);
    // Parent + name of `v` inside it (for getcwd reconstruction). The
    // parent comes back referenced; name_out is NAME_MAX+1 bytes.
    sint64_t (*getparent)(vnode* v, vnode** out_parent, char* name_out);

    // --- data ---
    // Read/write at a byte offset; *done receives the transferred count
    // (may be short at EOF). Never called with len == 0.
    sint64_t (*read)(vnode* v, uint64_t off, void* buf, uint64_t len,
                     uint64_t* done);
    sint64_t (*write)(vnode* v, uint64_t off, const void* buf, uint64_t len,
                      uint64_t* done);
    sint64_t (*truncate)(vnode* v, uint64_t size);

    // Directory stream. *cookie is a byte position in the logical entry
    // stream (d_off); the FS advances it past the returned entry and sets
    // *eof when there are no more. cookie 0 starts from the beginning.
    sint64_t (*readdir)(vnode* dir, uint64_t* cookie, dirent_out* out,
                        bool* eof);

    // --- metadata ---
    sint64_t (*getattr)(vnode* v, struct stat* st);
    // Only the permission bits of mode are honoured (FAT: READ_ONLY).
    sint64_t (*setattr)(vnode* v, uint32_t mode);
    sint64_t (*fsync)(vnode* v);
    sint64_t (*ioctl)(vnode* v, uint64_t request, uint64_t arg);

    // Readiness for blocking syscalls: true when read would not return
    // -EAGAIN right now (a tty line is complete, /dev/null is always
    // ready). null == always ready. The process layer pairs this with
    // Wait::Key + syscall restart.
    bool     (*poll_ready)(vnode* v);

    // Last reference dropped and the vnode is about to be freed: flush the
    // directory entry (size/time), free clusters if VF_UNLINKED.
    void     (*release)(vnode* v);
};

// Vnode flags
#define VF_UNLINKED     0x0001  // name gone; free clusters on release
#define VF_DIRTY        0x0002  // metadata changed, write entry on flush

struct vnode
{
    vtype        type;
    uint32_t     refcnt;        // one ref per user + one while cached
    uint64_t     size;
    uint32_t     mode;          // S_IFREG|perms etc. (abi/stat.h)
    uint64_t     mtime;         // epoch seconds (UTC)
    uint64_t     ctime;
    uint64_t     atime;

    mount*       mnt;           // filesystem this vnode belongs to
    vnode_ops*   ops;
    uint64_t     fs_key;        // identity within the FS (cache key)
    uint64_t     st_ino;        // synthetic inode number (from fs_key)
    uint16_t     flags;         // VF_*

    void*        fs_priv;       // FS-allocated per-vnode state
};

// A mounted filesystem instance.
struct mount
{
    vnode*  root;               // root vnode of the FS (ref held by mount)
    vnode*  point;              // directory mounted over (null: this is /)
    mount*  parent;             // mount that contains `point`
    vfs_fs* fs;                 // the driver behind it
    void*   fs_priv;            // driver state (fat_super* for fat32)
    char    devname[16];        // "usb0p1", "devfs", ...
    // Unique per mounted instance, handed out by mount_at. st_dev must
    // distinguish volumes, or (st_dev, st_ino) stops identifying a file and
    // "is this the same file?" checks across mounts go wrong.
    uint32_t dev_id;
    bool    active;
};

// One filesystem driver.
struct vfs_fs
{
    const char* name;           // "fat32", "devfs"
    vnode_ops*  ops;
    // Create (and reference) the root vnode for a new mount. `m` is the
    // mount slot being filled - use it as the vnode-cache key namespace.
    // `arg` is driver-specific (blkdev* for fat32, unused for devfs).
    sint64_t (*mount_fs)(mount* m, void* arg, vnode** out_root);
    // Flush everything, release driver state. Called by umount after the
    // vnode sweep, so no vnode of this FS exists any more.
    sint64_t (*umount_fs)(mount* m);
};

namespace vfs
{
    void init();

    // --- vnode cache / lifetime -------------------------------------------
    // Look up the cached vnode of `m` with identity `key`; on a miss create
    // one of `type` with `ops`, zero fs_priv. The caller owns one reference
    // either way (plus the cache's own reference).
    vnode* get_cached(mount* m, uint64_t key, vtype type, vnode_ops* ops,
                      sint64_t* out_rc);

    // Look up without creating: returns a referenced vnode or nullptr.
    vnode* find_cached(mount* m, uint64_t key);

    void   ref(vnode* v);
    void   unref(vnode* v);          // release at 0 refs, then free/evict

    // Remove `v` from the cache (its key becomes unreachable) without
    // freeing it: open references keep it alive until the last unref. Used
    // after unlink/rename, when the vnode's identity is no longer valid.
    void   invalidate(vnode* v);
    // Move a cached vnode to a new identity, keeping it cached. rename on a
    // filesystem whose key encodes the directory slot (FAT) needs this:
    // evicting instead would let a later open of the new path build a
    // *second* vnode for the same file, with its own size and offset caches.
    void   rekey(vnode* v, uint64_t new_key);

    // Mark metadata dirty; flushed by fsync/umount/sync/eviction.
    void   touch(vnode* v, bool mtime_now);

    // --- mounts -------------------------------------------------------------
    // point_dir: existing directory vnode to mount over (ref consumed), or
    // nullptr for the root mount. arg goes to fs->mount_fs.
    sint64_t mount_at(vnode* point_dir, const char* devname, vfs_fs* fs,
                      void* arg);
    // umount must refuse while a process still has the filesystem open, but
    // the VFS cannot see fd tables or process cwds from down here, and a
    // plain refcount test cannot either: drivers legitimately pin their own
    // vnodes (devfs keeps one per device for the life of the mount). So the
    // process layer registers a predicate at boot.
    typedef bool (*busy_hook_t)(mount* m);
    void set_busy_hook(busy_hook_t hook);

    // -EBUSY while any vnode of the FS is still referenced by an open fd or
    // a process cwd. `force` skips that check and is for the shutdown path
    // only, where no process is left to be surprised by the freed vnodes.
    sint64_t umount(mount* m, bool force = false);
    // Unmount every FS mounted on a directory belonging to `m` (deepest
    // first); call before umounting `m` itself.
    sint64_t umount_children(mount* m, bool force = false);
    mount*   root_mount();
    mount*   mount_count_get(uint32_t i);   // iterate: nullptr past the end
    uint32_t mount_count();

    // --- namei --------------------------------------------------------------
    // Resolve `path` (absolute, or relative to `cwd`) to a referenced vnode.
    // If `must_be_dir`, a trailing '/' or O_DIRECTORY-style mismatch gives
    // -ENOTDIR. namei walks through mount points in both directions.
    sint64_t lookup(const char* path, vnode* cwd, vnode** out, bool must_be_dir);

    // Resolve everything but the last component: *out_dir is referenced and
    // `name` (NAME_MAX+1 buffer) holds the final component. -ENOENT etc.
    sint64_t lookup_parent(const char* path, vnode* cwd, vnode** out_dir,
                           char* name);

    // Convenience wrappers used by the console until the fd layer lands.
    sint64_t open_path(const char* path, vnode* cwd, vnode** out);

    // Rebuild the absolute path of `v` into buf (size >= PATH_MAX); *len is
    // the string length. Walks .. up through mounts.
    sint64_t get_path(vnode* v, char* buf, uint32_t bufsize, uint32_t* len);

    // Flush every mount (sync).
    sint64_t sync_all();

    // --- system cwd ---------------------------------------------------------
    // One global current directory while the console drives the system
    // (stage 3.3). Stage 3.4 moves it into the Process struct; the root
    // process inherits this at launch. Automount sets it to the root vnode.
    //
    // cwd():    the vnode, NOT referenced - only valid while nothing can
    //           switch context (kernel main loop, syscall dispatch).
    // cwd_ref(): a referenced copy; unref it when done.
    // set_cwd(v): takes ownership of one reference to v.
    void     set_cwd(vnode* v);
    vnode*   cwd();
    vnode*   cwd_ref();
    // Absolute path of the cwd into buf (NUL-terminated); 0 or -errno.
    sint64_t cwd_path(char* buf, uint32_t bufsize);

    // Epoch seconds from the RTC (UTC): shared by the FS drivers for times.
    uint64_t now_epoch();
}

#endif // VFS_H
