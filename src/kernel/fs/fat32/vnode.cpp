// FAT32 vnode layer (stage 3.3): vnode_ops over the FAT/dir primitives,
// mount/umount, and everything the VFS calls.
//
// Identity (the vnode cache key). FAT has no inodes, so:
//   directory: its first cluster (unique per volume)   key = cluster<<32 | DIR_MARK
//   root:      root_cluster                            key = cluster<<32 | ROOT_MARK
//   file:      (parent cluster, physical slot)         key = cluster<<32 | slot
// Directories keyed by cluster resolve to the same vnode whether reached by
// name, by "." or by ".." - no scanning needed anywhere.
//
// st_ino is derived from the key. Two opens of one file share one vnode and
// therefore one size/chain cache.
//
// Permissions are emulated: directories 0755, files 0755 (exec checks only
// S_IFREG), READ_ONLY clears the write bits; uid = gid = 0; chmod toggles
// only READ_ONLY.

#include "../../../include/fs/fat32fs.h"
#include "../../../include/dev/bcache.h"
#include "../../../include/mm/heap.h"
#include "../../../include/mm/memory.h"
#include "../../../include/stdlib/string.h"
#include "../../../include/drivers/uart.h"
#include "../../../include/drivers/screen.h"
#include "../../../sdk/include/abi/errno.h"
#include "../../../sdk/include/abi/stat.h"
#include "../../../sdk/include/abi/dirent.h"
#include "../../../sdk/include/abi/fcntl.h"

// Forward declarations: the ops table below is filled in at file scope
// where all the static fat_* functions live.
static sint64_t fat_lookup(vnode* dir, const char* name, vnode** out);
static sint64_t fat_create(vnode* dir, const char* name, uint32_t mode, vnode** out);
static sint64_t fat_mkdir(vnode* dir, const char* name, uint32_t mode);
static sint64_t fat_unlink(vnode* dir, const char* name);
static sint64_t fat_rmdir(vnode* dir, const char* name);
static sint64_t fat_rename(vnode* old_dir, const char* old_name,
                           vnode* new_dir, const char* new_name, uint32_t flags);
static sint64_t fat_getparent(vnode* v, vnode** out_parent, char* name_out);
static sint64_t fat_read(vnode* v, uint64_t off, void* buf, uint64_t len,
                         uint64_t* done);
static sint64_t fat_write(vnode* v, uint64_t off, const void* buf, uint64_t len,
                          uint64_t* done);
static sint64_t fat_truncate(vnode* v, uint64_t size);
static sint64_t fat_readdir(vnode* dir, uint64_t* cookie, dirent_out* out,
                            bool* eof);
static sint64_t fat_getattr(vnode* v, struct stat* st);
static sint64_t fat_setattr(vnode* v, uint32_t mode);
static sint64_t fat_fsync(vnode* v);
static void     fat_release(vnode* v);

static vnode_ops fat_vnode_ops =
{
    fat_lookup, fat_create, fat_mkdir, fat_unlink, fat_rmdir,
    fat_rename, fat_getparent,
    fat_read, fat_write, fat_truncate, fat_readdir,
    fat_getattr, fat_setattr, fat_fsync,
    nullptr,            // ioctl: files have none
    nullptr,            // poll_ready: regular files are always ready
    fat_release,
};

namespace
{
    const uint32_t DIR_MARK  = 0xFFFFFFFEu;
    const uint32_t ROOT_MARK = 0xFFFFFFFFu;
    const uint32_t MAX_FAT_VOLUMES = 4;

    fat_super supers[MAX_FAT_VOLUMES];

    uint64_t file_key(uint32_t parent_cluster, uint32_t slot)
    {
        return ((uint64_t)parent_cluster << 32) | slot;
    }
    uint64_t dir_key(uint32_t first_cluster)
    {
        return ((uint64_t)first_cluster << 32) | DIR_MARK;
    }
    uint64_t root_key(uint32_t root_cluster)
    {
        return ((uint64_t)root_cluster << 32) | ROOT_MARK;
    }

    uint32_t mode_for(bool is_dir, uint8_t attr)
    {
        uint32_t perms = (attr & FAT_ATTR_READ_ONLY) ? 0555 : 0755;
        return (is_dir ? S_IFDIR : S_IFREG) | perms;
    }

    uint32_t entry_cluster(const fat_dir_entry* e)
    {
        return ((uint32_t)e->first_cluster_hi << 16) | e->first_cluster_lo;
    }


    // -----------------------------------------------------------------------
    // vnode construction
    // -----------------------------------------------------------------------

    // Allocate and attach the fat_node of a freshly created vnode.
    fat_node* attach_node(vnode* v, fat_super* sb, uint32_t first_cluster,
                          uint32_t parent_cluster, uint32_t entry_index,
                          uint64_t entry_lba, uint32_t entry_off)
    {
        fat_node* fn = (fat_node*)kmalloc(sizeof(fat_node));
        if (!fn)
            return nullptr;
        memory::memset((uint8_t*)fn, 0, sizeof(fat_node));
        fn->sb             = sb;
        fn->first_cluster  = first_cluster;
        fn->parent_cluster = parent_cluster;
        fn->entry_index    = entry_index;
        fn->entry_lba      = entry_lba;
        fn->entry_off      = entry_off;
        v->fs_priv = fn;
        return fn;
    }

    // Fetch (or create) the vnode of a directory by its first cluster.
    sint64_t dir_vnode(fat_super* sb, uint32_t first_cluster, vnode** out)
    {
        if (first_cluster == sb->root_cluster || first_cluster < 2)
        {
            sint64_t rc = 0;
            vnode* v = vfs::get_cached(sb->mnt, root_key(sb->root_cluster),
                                       vtype::DIR, &fat_vnode_ops, &rc);
            if (!v)
                return rc;
            if (!v->fs_priv)
            {
                if (!attach_node(v, sb, sb->root_cluster, sb->root_cluster,
                                 ROOT_MARK, 0, 0))
                {
                    vfs::unref(v);
                    return -ENOMEM;
                }
                v->size = 0;
                v->mode = S_IFDIR | 0755;
                v->mtime = vfs::now_epoch();
            }
            *out = v;
            return 0;
        }

        sint64_t rc = 0;
        vnode* v = vfs::get_cached(sb->mnt, dir_key(first_cluster),
                                   vtype::DIR, &fat_vnode_ops, &rc);
        if (!v)
            return rc;
        if (!v->fs_priv)
        {
            if (!attach_node(v, sb, first_cluster, 0, DIR_MARK, 0, 0))
            {
                vfs::unref(v);
                return -ENOMEM;
            }
            v->size = 0;
            v->mode = S_IFDIR | 0755;
            v->mtime = vfs::now_epoch();
        }
        *out = v;
        return 0;
    }

    // Fetch (or create) the vnode of a directory record.
    sint64_t vnode_from_record(fat_super* sb, const fat_dirent* rec,
                               uint32_t parent_cluster, uint32_t slot,
                               uint64_t entry_lba, uint32_t entry_off,
                               vnode** out)
    {
        bool is_dir = (rec->e.attr & FAT_ATTR_DIRECTORY) != 0;
        uint32_t fc = entry_cluster(&rec->e);

        if (is_dir)
            return dir_vnode(sb, fc, out);

        sint64_t rc = 0;
        vnode* v = vfs::get_cached(sb->mnt, file_key(parent_cluster, slot),
                                   vtype::REG, &fat_vnode_ops, &rc);
        if (!v)
            return rc;

        if (!v->fs_priv)
        {
            if (!attach_node(v, sb, fc, parent_cluster, slot,
                             entry_lba, entry_off))
            {
                vfs::unref(v);
                return -ENOMEM;
            }
            v->size  = rec->e.file_size;
            v->mode  = mode_for(false, rec->e.attr);
            v->mtime = fatdir::fat_to_epoch(rec->e.write_date, rec->e.write_time);
            v->atime = fatdir::fat_to_epoch(rec->e.last_access_date, 0);
            v->ctime = fatdir::fat_to_epoch(rec->e.create_date, rec->e.create_time);
        }

        *out = v;
        return 0;
    }

    // Read the ".." record of directory cluster `cluster`; returns the
    // parent's first cluster (root_cluster when it is the root).
    sint64_t dotdot_cluster(fat_super* sb, uint32_t cluster, uint32_t* out)
    {
        if (cluster == sb->root_cluster)
        {
            *out = sb->root_cluster;
            return 0;
        }

        uint64_t lba = fat::cluster_lba(sb, cluster);
        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, lba, &rc);
        if (!b)
            return rc;

        const fat_dir_entry* dd = (const fat_dir_entry*)(b->data + 32);
        uint32_t pc = entry_cluster(dd);
        bcache::put(b, false);

        *out = (pc >= 2) ? pc : sb->root_cluster;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// vnode_ops
// ---------------------------------------------------------------------------

static sint64_t fat_lookup(vnode* dir, const char* name, vnode** out)
{
    // "." and ".." never arrive here: namei::step handles them via
    // getparent/the mount table.
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    fat_dirent rec;
    uint32_t slot = 0;
    bool found = false;
    uint64_t entry_lba = 0;
    uint32_t entry_off = 0;

    sint64_t rc = fatdir::find(dn, name, &rec, &slot, &found,
                               &entry_lba, &entry_off);
    if (rc != 0)
        return rc;
    if (!found)
        return -ENOENT;

    return vnode_from_record(sb, &rec, dn->first_cluster, slot,
                             entry_lba, entry_off, out);
}

static sint64_t fat_create(vnode* dir, const char* name, uint32_t mode,
                           vnode** out)
{
    (void)mode;
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    uint32_t new_slot = 0;
    uint64_t entry_lba = 0;
    uint32_t entry_off = 0;

    sint64_t rc = fatdir::create(dn, name, false, 0,
                                 &new_slot, &entry_lba, &entry_off);
    if (rc != 0)
        return rc;

    // Build the vnode straight from what we wrote: no re-read needed.
    rc = 0;
    vnode* v = vfs::get_cached(sb->mnt, file_key(dn->first_cluster, new_slot),
                               vtype::REG, &fat_vnode_ops, &rc);
    if (!v)
        return rc;

    if (!v->fs_priv)
    {
        if (!attach_node(v, sb, 0, dn->first_cluster, new_slot,
                         entry_lba, entry_off))
        {
            vfs::unref(v);
            return -ENOMEM;
        }
        v->size  = 0;
        v->mode  = S_IFREG | (mode & 0777);
        v->mtime = vfs::now_epoch();
        v->ctime = v->mtime;
        v->atime = v->mtime;
    }

    vfs::touch(dir, true);
    *out = v;
    return 0;
}

static sint64_t fat_mkdir(vnode* dir, const char* name, uint32_t mode)
{
    (void)mode;
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    uint32_t newc = 0;
    sint64_t rc = fat::alloc_cluster(sb, &newc);
    if (rc != 0)
        return rc;

    // "." and ".." in the first cluster; the rest of the cluster zeroed
    // (a directory must start with the 0x00 end marker after "..").
    memory::memset(sb->scratch, 0, sb->cluster_size);
    uint16_t date = 0, time = 0;
    fatdir::epoch_to_fat(vfs::now_epoch(), &date, &time);

    fat_dir_entry* dot = (fat_dir_entry*)sb->scratch;
    memory::memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->create_time = dot->write_time = time;
    dot->create_date = dot->write_date = dot->last_access_date = date;
    dot->first_cluster_hi = (uint16_t)(newc >> 16);
    dot->first_cluster_lo = (uint16_t)(newc & 0xFFFF);

    fat_dir_entry* dotdot = (fat_dir_entry*)(sb->scratch + 32);
    memory::memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->create_time = dotdot->write_time = time;
    dotdot->create_date = dotdot->write_date = dotdot->last_access_date = date;
    dotdot->first_cluster_hi = (uint16_t)(dn->first_cluster >> 16);
    dotdot->first_cluster_lo = (uint16_t)(dn->first_cluster & 0xFFFF);

    rc = fat::write_cluster(sb, newc, sb->scratch);
    if (rc != 0)
    {
        fat::free_chain(sb, newc);
        return rc;
    }

    uint32_t new_slot = 0;
    rc = fatdir::create(dn, name, true, newc, &new_slot, nullptr, nullptr);
    if (rc != 0)
    {
        fat::free_chain(sb, newc);
        return rc;
    }

    vfs::touch(dir, true);
    return 0;
}

static sint64_t fat_unlink(vnode* dir, const char* name)
{
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    fat_dirent rec;
    uint32_t slot = 0;
    bool found = false;
    sint64_t rc = fatdir::find(dn, name, &rec, &slot, &found, nullptr, nullptr);
    if (rc != 0)
        return rc;
    if (!found)
        return -ENOENT;
    if (rec.e.attr & FAT_ATTR_DIRECTORY)
        return -EISDIR;

    // A cached vnode keeps working through open fds: mark it unlinked so
    // the clusters are freed at the last release (POSIX temp-file pattern).
    // Without a cached vnode nobody holds the clusters: free them now.
    vnode* v = vfs::find_cached(sb->mnt, file_key(dn->first_cluster, slot));
    if (v)
    {
        v->flags |= VF_UNLINKED;
        vfs::unref(v);
    }
    else
    {
        uint32_t fc = entry_cluster(&rec.e);
        if (fc >= 2 && fc < FAT_CLUSTER_EOC)
        {
            rc = fat::free_chain(sb, fc);
            if (rc != 0)
                return rc;
        }
    }

    rc = fatdir::remove(dn, slot);
    if (rc != 0)
        return rc;

    vfs::touch(dir, true);
    return 0;
}

// Is directory cluster `child` the same as or inside directory cluster
// `ancestor`? Used by rmdir/rename self-containment checks.
static sint64_t dir_contains(fat_super* sb, uint32_t ancestor, uint32_t child,
                             bool* out)
{
    *out = false;
    uint32_t cur = child;
    uint32_t guard = 0;

    for (;;)
    {
        if (cur == ancestor)
        {
            *out = true;
            return 0;
        }
        if (cur == sb->root_cluster)
            return 0;

        uint32_t pc = 0;
        sint64_t rc = dotdot_cluster(sb, cur, &pc);
        if (rc != 0)
            return rc;
        if (pc == cur)
            return 0;           // loop guard: broken ".."
        cur = pc;

        if (++guard > sb->total_clusters)
            return -EIO;        // corrupt ".." chain
    }
}

static sint64_t fat_rmdir(vnode* dir, const char* name)
{
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    fat_dirent rec;
    uint32_t slot = 0;
    bool found = false;
    sint64_t rc = fatdir::find(dn, name, &rec, &slot, &found, nullptr, nullptr);
    if (rc != 0)
        return rc;
    if (!found)
        return -ENOENT;
    if (!(rec.e.attr & FAT_ATTR_DIRECTORY))
        return -ENOTDIR;

    uint32_t target = entry_cluster(&rec.e);
    if (target < 2)
        return -EIO;

    // Must be empty: only "." and "..".
    fat_node tn;
    memory::memset((uint8_t*)&tn, 0, sizeof(tn));
    tn.sb = sb;
    tn.first_cluster = target;

    uint64_t cookie = 0;
    for (;;)
    {
        fat_dirent e;
        uint32_t eslot = 0;
        bool eof = false;
        rc = fatdir::next_record(&tn, cookie, &e, &eslot, &cookie, &eof);
        if (rc != 0)
            return rc;
        if (eof)
            break;
        if (strcmp(e.lfn, ".") != 0 && strcmp(e.lfn, "..") != 0)
            return -ENOTEMPTY;
    }

    // A cached vnode of the removed directory must not resurrect it. The
    // chain is freed here, so release() must not free it a second time:
    // zero first_cluster after marking.
    vnode* v = vfs::find_cached(sb->mnt, dir_key(target));
    if (v)
    {
        v->flags |= VF_UNLINKED;
        fat_node* vn = (fat_node*)v->fs_priv;
        vn->first_cluster = 0;
        vfs::unref(v);
    }

    rc = fatdir::remove(dn, slot);
    if (rc != 0)
        return rc;

    rc = fat::free_chain(sb, target);
    if (rc != 0)
        return rc;

    vfs::touch(dir, true);
    return 0;
}

static sint64_t fat_rename(vnode* old_dir, const char* old_name,
                           vnode* new_dir, const char* new_name,
                           uint32_t flags)
{
    if (flags & RENAME_EXCHANGE)
        return -EINVAL;             // not supported on FAT

    fat_node* odn = (fat_node*)old_dir->fs_priv;
    fat_node* ndn = (fat_node*)new_dir->fs_priv;
    fat_super* sb = odn->sb;

    if (strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
        strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0)
        return -EINVAL;

    fat_dirent src;
    uint32_t src_slot = 0;
    bool src_found = false;
    sint64_t rc = fatdir::find(odn, old_name, &src, &src_slot, &src_found,
                               nullptr, nullptr);
    if (rc != 0)
        return rc;
    if (!src_found)
        return -ENOENT;

    bool src_is_dir = (src.e.attr & FAT_ATTR_DIRECTORY) != 0;
    uint32_t src_cluster = entry_cluster(&src.e);

    if (src_is_dir)
    {
        // The new parent must not be the moved directory or inside it.
        bool inside = false;
        rc = dir_contains(sb, src_cluster, ndn->first_cluster, &inside);
        if (rc != 0)
            return rc;
        if (inside)
            return -EINVAL;
    }

    // Existing target?
    fat_dirent dst;
    uint32_t dst_slot = 0;
    bool dst_found = false;
    rc = fatdir::find(ndn, new_name, &dst, &dst_slot, &dst_found,
                      nullptr, nullptr);
    if (rc != 0)
        return rc;

    if (dst_found)
    {
        if (flags & RENAME_NOREPLACE)
            return -EEXIST;

        bool dst_is_dir = (dst.e.attr & FAT_ATTR_DIRECTORY) != 0;
        if (dst_is_dir != src_is_dir)
            return dst_is_dir ? -EISDIR : -ENOTDIR;

        uint32_t dst_cluster = entry_cluster(&dst.e);
        if (dst_is_dir)
        {
            if (dst_cluster == sb->root_cluster)
                return -EINVAL;

            // Must be empty.
            fat_node tn;
            memory::memset((uint8_t*)&tn, 0, sizeof(tn));
            tn.sb = sb;
            tn.first_cluster = dst_cluster;

            uint64_t cookie = 0;
            for (;;)
            {
                fat_dirent e;
                uint32_t eslot = 0;
                bool eof = false;
                rc = fatdir::next_record(&tn, cookie, &e, &eslot, &cookie, &eof);
                if (rc != 0)
                    return rc;
                if (eof)
                    break;
                if (strcmp(e.lfn, ".") != 0 && strcmp(e.lfn, "..") != 0)
                    return -ENOTEMPTY;
            }
        }

        vnode* dv = vfs::find_cached(sb->mnt,
                                     dst_is_dir ? dir_key(dst_cluster)
                                                : file_key(ndn->first_cluster, dst_slot));
        if (dv)
        {
            dv->flags |= VF_UNLINKED;
            if (dst_is_dir)
            {
                // The directory chain is freed below: keep release() from
                // freeing it a second time.
                fat_node* dvn = (fat_node*)dv->fs_priv;
                dvn->first_cluster = 0;
            }
            vfs::unref(dv);
        }

        rc = fatdir::remove(ndn, dst_slot);
        if (rc != 0)
            return rc;

        if (dst_is_dir)
        {
            rc = fat::free_chain(sb, dst_cluster);
            if (rc != 0)
                return rc;
        }
        else if (dst_cluster >= 2 && dst_cluster < FAT_CLUSTER_EOC && !dv)
        {
            // No cached vnode holds the replaced file: free its clusters
            // now. A cached one is marked VF_UNLINKED above and frees them
            // at its last release.
            rc = fat::free_chain(sb, dst_cluster);
            if (rc != 0)
                return rc;
        }
    }

    // Create the new record pointing at the same clusters.
    uint32_t new_slot = 0;
    uint64_t new_lba = 0;
    uint32_t new_off = 0;
    rc = fatdir::create(ndn, new_name, src_is_dir, src_cluster,
                        &new_slot, &new_lba, &new_off);
    if (rc != 0)
        return rc;

    // Carry the size and creation time over for files; fix ".." for dirs.
    if (!src_is_dir && src.e.file_size)
    {
        sint64_t rc2 = 0;
        buf* b = bcache::get(sb->dev, new_lba, &rc2);
        if (b)
        {
            fat_dir_entry* e = (fat_dir_entry*)(b->data + new_off);
            e->file_size = src.e.file_size;
            e->create_time = src.e.create_time;
            e->create_date = src.e.create_date;
            e->create_time_tenth = src.e.create_time_tenth;
            bcache::put(b, true);
        }
    }
    if (src_is_dir)
    {
        uint64_t lba = fat::cluster_lba(sb, src_cluster);
        sint64_t rc2 = 0;
        buf* b = bcache::get(sb->dev, lba, &rc2);
        if (b)
        {
            fat_dir_entry* dd = (fat_dir_entry*)(b->data + 32);
            dd->first_cluster_hi = (uint16_t)(ndn->first_cluster >> 16);
            dd->first_cluster_lo = (uint16_t)(ndn->first_cluster & 0xFFFF);
            bcache::put(b, true);
        }
    }

    rc = fatdir::remove(odn, src_slot);
    if (rc != 0)
        return rc;

    // Fix or evict the cached vnode of the moved object.
    if (src_is_dir)
    {
        // Key is cluster-based: the vnode stays valid, only its record
        // location and parent changed.
        vnode* v = vfs::find_cached(sb->mnt, dir_key(src_cluster));
        if (v)
        {
            fat_node* fn = (fat_node*)v->fs_priv;
            fn->parent_cluster = ndn->first_cluster;
            fn->entry_index = new_slot;
            fn->entry_lba = new_lba;
            fn->entry_off = new_off;
            vfs::unref(v);
        }
    }
    else
    {
        // File key includes (parent, slot): the identity changed. Evict the
        // old vnode from the cache (open fds keep their reference and get
        // the new record location so writes still land correctly).
        vnode* v = vfs::find_cached(sb->mnt,
                                    file_key(odn->first_cluster, src_slot));
        if (v)
        {
            fat_node* fn = (fat_node*)v->fs_priv;
            fn->parent_cluster = ndn->first_cluster;
            fn->entry_index = new_slot;
            fn->entry_lba = new_lba;
            fn->entry_off = new_off;
            vfs::invalidate(v);
            vfs::unref(v);
        }
    }

    vfs::touch(old_dir, true);
    vfs::touch(new_dir, true);
    return 0;
}

static sint64_t fat_getparent(vnode* v, vnode** out_parent, char* name_out)
{
    fat_node* fn = (fat_node*)v->fs_priv;
    fat_super* sb = fn->sb;

    name_out[0] = '\0';

    // The root's parent is itself: namei treats that as the top.
    if (fn->entry_index == ROOT_MARK)
    {
        vfs::ref(v);
        *out_parent = v;
        return 0;
    }

    uint32_t parent_cluster;
    if (fn->entry_index == DIR_MARK)
    {
        // Directory keyed by cluster: its parent comes from its "..".
        sint64_t rc = dotdot_cluster(sb, fn->first_cluster, &parent_cluster);
        if (rc != 0)
            return rc;
    }
    else
    {
        parent_cluster = fn->parent_cluster;
    }

    sint64_t rc = dir_vnode(sb, parent_cluster, out_parent);
    if (rc != 0)
        return rc;

    // The name: scan the parent for our record (getcwd is the only user).
    fat_node pn;
    memory::memset((uint8_t*)&pn, 0, sizeof(pn));
    pn.sb = sb;
    pn.first_cluster = parent_cluster;

    uint64_t cookie = 0;
    for (;;)
    {
        fat_dirent rec;
        uint32_t slot = 0;
        bool eof = false;
        rc = fatdir::next_record(&pn, cookie, &rec, &slot, &cookie, &eof);
        if (rc != 0)
        {
            vfs::unref(*out_parent);
            *out_parent = nullptr;
            return rc;
        }
        if (eof)
            break;

        bool match = (fn->entry_index == DIR_MARK)
                   ? (entry_cluster(&rec.e) == fn->first_cluster &&
                      (rec.e.attr & FAT_ATTR_DIRECTORY))
                   : (slot == fn->entry_index);
        if (match)
        {
            strncpy(name_out, rec.lfn, NAME_MAX);
            name_out[NAME_MAX] = '\0';
            break;
        }
    }

    if (name_out[0] == '\0' && fn->entry_index != DIR_MARK && fn->entry_lba)
    {
        // Record vanished mid-scan (concurrent unlink): fall back to the
        // raw short name so getcwd still produces something parseable.
        sint64_t rc2 = 0;
        buf* b = bcache::get(sb->dev, fn->entry_lba, &rc2);
        if (b)
        {
            fat_dir_entry e;
            memory::memcpy((uint8_t*)&e, b->data + fn->entry_off, 32);
            bcache::put(b, false);
            if (e.name[0] != FAT_DIR_FREE)
                fatdir::format_short_name(&e, name_out);
        }
    }

    return 0;
}

static sint64_t fat_read(vnode* v, uint64_t off, void* buf, uint64_t len,
                         uint64_t* done)
{
    if (v->type != vtype::REG)
        return -EISDIR;
    fat_node* fn = (fat_node*)v->fs_priv;
    return fat::read_at(v, fn, off, buf, len, done);
}

static sint64_t fat_write(vnode* v, uint64_t off, const void* buf, uint64_t len,
                          uint64_t* done)
{
    if (v->type != vtype::REG)
        return -EISDIR;
    fat_node* fn = (fat_node*)v->fs_priv;

    sint64_t rc = fat::write_at(v, fn, off, buf, len, done);
    if (rc != 0)
        return rc;

    // The record's size/first-cluster follow the data (an empty file got
    // its first cluster here).
    if (fn->entry_index < DIR_MARK)
        rc = fatdir::update_entry(v, fn);
    return rc;
}

static sint64_t fat_truncate(vnode* v, uint64_t size)
{
    if (v->type != vtype::REG)
        return -EISDIR;
    fat_node* fn = (fat_node*)v->fs_priv;

    sint64_t rc = fat::truncate(v, fn, size);
    if (rc != 0)
        return rc;
    if (fn->entry_index < DIR_MARK)
        rc = fatdir::update_entry(v, fn);
    return rc;
}

static sint64_t fat_readdir(vnode* dir, uint64_t* cookie, dirent_out* out,
                            bool* eof)
{
    fat_node* dn = (fat_node*)dir->fs_priv;
    fat_super* sb = dn->sb;

    // Cookies are physical slot numbers; the getdents64 layer above
    // synthesizes "." and "..". The "." / ".." records of subdirectories
    // are skipped here so every directory stream looks the same.
    for (;;)
    {
        fat_dirent rec;
        uint32_t slot = 0;
        sint64_t rc = fatdir::next_record(dn, *cookie, &rec, &slot,
                                          cookie, eof);
        if (rc != 0 || *eof)
            return rc;

        if (strcmp(rec.lfn, ".") == 0 || strcmp(rec.lfn, "..") == 0)
            continue;

        uint32_t fc = entry_cluster(&rec.e);
        bool is_dir = (rec.e.attr & FAT_ATTR_DIRECTORY) != 0;

        out->ino = is_dir ? (fc == sb->root_cluster ? root_key(fc) : dir_key(fc))
                          : file_key(dn->first_cluster, slot);
        out->type = is_dir ? DT_DIR : DT_REG;
        strncpy(out->name, rec.lfn, NAME_MAX);
        out->name[NAME_MAX] = '\0';
        return 0;
    }
}

static sint64_t fat_getattr(vnode* v, struct stat* st)
{
    fat_node* fn = (fat_node*)v->fs_priv;
    fat_super* sb = fn->sb;

    memory::memset((uint8_t*)st, 0, sizeof(struct stat));
    st->st_dev     = 1;
    st->st_ino     = v->st_ino;
    st->st_nlink   = 1;
    st->st_mode    = v->mode;
    st->st_uid     = 0;
    st->st_gid     = 0;
    st->st_size    = (v->type == vtype::DIR) ? 0 : (sint64_t)v->size;
    st->st_blksize = (sint64_t)sb->cluster_size;
    st->st_blocks  = (v->type == vtype::DIR)
                   ? 0
                   : (sint64_t)((v->size + sb->cluster_size - 1) / sb->cluster_size) *
                     (sb->cluster_size / 512);
    st->st_atim.tv_sec = (sint64_t)v->atime;
    st->st_mtim.tv_sec = (sint64_t)v->mtime;
    st->st_ctim.tv_sec = (sint64_t)(v->ctime ? v->ctime : v->mtime);
    return 0;
}

static sint64_t fat_setattr(vnode* v, uint32_t mode)
{
    fat_node* fn = (fat_node*)v->fs_priv;
    fat_super* sb = fn->sb;

    // Only the write bits are representable (READ_ONLY attribute).
    if (fn->entry_index < DIR_MARK && fn->entry_lba)
    {
        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, fn->entry_lba, &rc);
        if (!b)
            return rc;

        fat_dir_entry* e = (fat_dir_entry*)(b->data + fn->entry_off);
        if (mode & S_IWUSR)
            e->attr &= (uint8_t)~FAT_ATTR_READ_ONLY;
        else
            e->attr |= FAT_ATTR_READ_ONLY;
        bcache::put(b, true);
    }

    v->mode = (v->mode & S_IFMT) | (mode & 0777);
    return 0;
}

static sint64_t fat_fsync(vnode* v)
{
    fat_node* fn = (fat_node*)v->fs_priv;
    fat_super* sb = fn->sb;

    if (v->type == vtype::REG && (v->flags & VF_DIRTY) &&
        fn->entry_index < DIR_MARK)
    {
        sint64_t rc = fatdir::update_entry(v, fn);
        if (rc != 0)
            return rc;
    }

    sint64_t rc = fat::flush_fsinfo(sb);
    if (rc != 0)
        return rc;
    return bcache::flush(sb->dev);
}

static void fat_release(vnode* v)
{
    fat_node* fn = (fat_node*)v->fs_priv;
    if (!fn)
        return;

    fat_super* sb = fn->sb;

    // Write back pending metadata while the entry still exists.
    if ((v->flags & VF_DIRTY) && v->type == vtype::REG &&
        fn->entry_index < DIR_MARK && !(v->flags & VF_UNLINKED))
        fatdir::update_entry(v, fn);

    if (v->flags & VF_UNLINKED)
    {
        // Name is gone: the clusters belong to nobody now.
        if (fn->first_cluster >= 2)
        {
            fat::free_chain(sb, fn->first_cluster);
            fat::flush_fsinfo(sb);
        }
    }
}

// ---------------------------------------------------------------------------
// mount / umount
// ---------------------------------------------------------------------------

namespace
{
    sint64_t read_super(fat_super* sb, mount* m, blkdev* dev)
    {
        memory::memset((uint8_t*)sb, 0, sizeof(fat_super));
        sb->dev = dev;
        sb->mnt = m;

        sint64_t rc = 0;
        buf* b = bcache::get(dev, 0, &rc);
        if (!b)
            return rc;

        const fat_bpb* bpb = (const fat_bpb*)b->data;

        if (bpb->bytes_per_sector != dev->sector_size)
        {
            bcache::put(b, false);
            uart::printf("fat32: %s: BPB sector %u != device %u\n",
                         dev->name, (uint32_t)bpb->bytes_per_sector,
                         dev->sector_size);
            return -EINVAL;
        }
        uint8_t spc = bpb->sectors_per_cluster;
        if (spc == 0 || spc > 128 || (spc & (spc - 1)))
        {
            bcache::put(b, false);
            return -EINVAL;
        }
        if (bpb->num_fats == 0 || bpb->fat_size_32 == 0 ||
            bpb->root_cluster < 2 || bpb->total_sectors_32 == 0)
        {
            bcache::put(b, false);
            uart::printf("fat32: %s: not a FAT32 volume\n", dev->name);
            return -EINVAL;
        }

        sb->bytes_per_sector    = bpb->bytes_per_sector;
        sb->sectors_per_cluster = spc;
        sb->cluster_size        = (uint32_t)spc * bpb->bytes_per_sector;
        sb->reserved_sectors    = bpb->reserved_sectors;
        sb->num_fats            = bpb->num_fats;
        sb->fat_size            = bpb->fat_size_32;
        sb->fat_start           = bpb->reserved_sectors;
        sb->data_start          = bpb->reserved_sectors +
                                  (uint32_t)bpb->num_fats * bpb->fat_size_32;
        sb->root_cluster        = bpb->root_cluster;
        sb->total_sectors       = bpb->total_sectors_32;
        sb->fsinfo_sector       = bpb->fs_info_sector;

        // The device is the authority on the volume size.
        if ((uint64_t)sb->total_sectors > dev->sector_count)
            sb->total_sectors = (uint32_t)dev->sector_count;

        if ((uint64_t)sb->data_start >= sb->total_sectors)
        {
            bcache::put(b, false);
            uart::printf("fat32: %s: data area beyond the volume\n", dev->name);
            return -EINVAL;
        }

        sb->total_clusters = (sb->total_sectors - sb->data_start) / spc;
        bcache::put(b, false);

        sb->scratch = (uint8_t*)kmalloc(sb->cluster_size);
        if (!sb->scratch)
            return -ENOMEM;

        fat::load_fsinfo(sb);

        // Warn on a dirty volume (FAT[1] bit 27); fsck is out of scope.
        uint32_t fat1 = 0;
        if (fat::read_entry(sb, 1, &fat1) == 0 && (fat1 & FAT_VOL_DIRTY))
        {
            uart::printf("fat32: %s: volume is dirty (unclean unmount)\n",
                         dev->name);
            screen::printf("Warning: %s was not unmounted cleanly.\n\r",
                           dev->name);
        }
        fat::set_dirty_bit(sb, true);

        sb->active = true;
        return 0;
    }

    sint64_t fat_mount_fs(mount* m, void* arg, vnode** out_root)
    {
        blkdev* dev = (blkdev*)arg;
        if (!dev)
            return -EINVAL;

        // Refuse to mount one blkdev twice: two supers over the same sectors
        // would disagree about caches.
        for (uint32_t i = 0; i < MAX_FAT_VOLUMES; i++)
            if (supers[i].active && supers[i].dev == dev)
                return -EBUSY;

        fat_super* sb = nullptr;
        for (uint32_t i = 0; i < MAX_FAT_VOLUMES; i++)
            if (!supers[i].active)
            {
                sb = &supers[i];
                break;
            }
        if (!sb)
            return -ENFILE;

        sint64_t rc = read_super(sb, m, dev);
        if (rc != 0)
        {
            memory::memset((uint8_t*)sb, 0, sizeof(fat_super));
            return rc;
        }

        // Sanity: the root cluster must be allocated (a free one means this
        // is not a formatted FAT32 volume).
        uint32_t v = 0;
        rc = fat::read_entry(sb, sb->root_cluster, &v);
        if (rc != 0 || v == FAT_CLUSTER_FREE)
        {
            kfree(sb->scratch);
            memory::memset((uint8_t*)sb, 0, sizeof(fat_super));
            return rc != 0 ? rc : -EINVAL;
        }

        vnode* root = nullptr;
        rc = dir_vnode(sb, sb->root_cluster, &root);
        if (rc != 0)
        {
            kfree(sb->scratch);
            memory::memset((uint8_t*)sb, 0, sizeof(fat_super));
            return rc;
        }

        m->fs_priv = sb;

        uart::printf("fat32: %s mounted: cluster=%u clusters=%u free=%u\n",
                     dev->name, sb->cluster_size, sb->total_clusters,
                     sb->free_count);

        *out_root = root;
        return 0;
    }

    sint64_t fat_umount_fs(mount* m)
    {
        fat_super* sb = (fat_super*)m->fs_priv;
        if (!sb || !sb->active)
            return -EINVAL;

        fat::flush_fsinfo(sb);
        fat::set_dirty_bit(sb, false);
        bcache::release(sb->dev);

        if (sb->scratch)
            kfree(sb->scratch);
        memory::memset((uint8_t*)sb, 0, sizeof(fat_super));
        m->fs_priv = nullptr;
        return 0;
    }
}

namespace fat32fs
{
    vfs_fs fs =
    {
        "fat32",
        &fat_vnode_ops,
        fat_mount_fs,
        fat_umount_fs,
    };
}
