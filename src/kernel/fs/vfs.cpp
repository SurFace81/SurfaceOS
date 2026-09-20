// VFS core: vnode cache, mount table, path resolution (stage 3.3).
// See vfs.h for the contract. namei lives here too - it is small and shares
// the mount/vnode plumbing.

#include "../../include/fs/vfs.h"
#include "../../include/dev/bcache.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/rtc.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/dirent.h"

namespace
{
    const uint32_t MAX_VNODES = 256;
    const uint32_t MAX_MOUNTS = 8;

    vnode* vnode_pool[MAX_VNODES];      // cached vnodes (each holds 1 ref)
    uint32_t vnode_count = 0;

    mount mounts[MAX_MOUNTS];
    uint32_t mount_cnt = 0;

    // Days from 1970-01-01 (Civil From Days, Howard Hinnant).
    sint64_t days_from_civil(int y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);            // [0,399]
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097LL + (sint64_t)doe - 719468LL;
    }

    // Evict an unreferenced (refcnt == 1: only the cache's) clean vnode.
    vnode* find_evictable()
    {
        for (uint32_t i = 0; i < MAX_VNODES; i++)
        {
            vnode* v = vnode_pool[i];
            if (v && v->refcnt == 1 && !(v->flags & VF_DIRTY))
                return v;
        }
        return nullptr;
    }
}

namespace vfs
{
    void init()
    {
        memory::memset((uint8_t*)vnode_pool, 0, sizeof(vnode_pool));
        memory::memset((uint8_t*)mounts, 0, sizeof(mounts));
        vnode_count = 0;
        mount_cnt = 0;
    }

    uint64_t now_epoch()
    {
        rtc_time t;
        rtc::read(&t);
        if (t.year < 1970 || t.month < 1 || t.month > 12 || t.day < 1)
            return 0;
        sint64_t days = days_from_civil((int)t.year, t.month, t.day);
        return (uint64_t)(days * 86400LL + (sint64_t)t.hours * 3600 +
                          (sint64_t)t.minutes * 60 + (sint64_t)t.seconds);
    }

    // -----------------------------------------------------------------------
    // vnode cache
    // -----------------------------------------------------------------------

    vnode* get_cached(mount* m, uint64_t key, vtype type, vnode_ops* ops,
                      sint64_t* out_rc)
    {
        // Hit: same FS + same identity.
        for (uint32_t i = 0; i < MAX_VNODES; i++)
        {
            vnode* v = vnode_pool[i];
            if (v && v->mnt == m && v->fs_key == key)
            {
                v->refcnt++;
                if (out_rc) *out_rc = 0;
                return v;
            }
        }

        // Miss: allocate a slot, evicting a clean unreferenced vnode.
        int slot = -1;
        for (uint32_t i = 0; i < MAX_VNODES; i++)
            if (!vnode_pool[i])
            {
                slot = (int)i;
                break;
            }

        if (slot < 0)
        {
            vnode* victim = find_evictable();
            if (!victim)
            {
                if (out_rc) *out_rc = -ENFILE;
                return nullptr;
            }
            if (victim->ops->release)
                victim->ops->release(victim);
            // The victim held the cache's own reference: drop it and free.
            victim->refcnt--;
            if (victim->fs_priv)
                kfree(victim->fs_priv);
            kfree(victim);
            for (uint32_t i = 0; i < MAX_VNODES; i++)
                if (vnode_pool[i] == victim)
                {
                    vnode_pool[i] = nullptr;
                    slot = (int)i;
                    break;
                }
        }

        vnode* v = (vnode*)kmalloc(sizeof(vnode));
        if (!v)
        {
            if (out_rc) *out_rc = -ENOMEM;
            return nullptr;
        }
        memory::memset((uint8_t*)v, 0, sizeof(vnode));

        v->type    = type;
        v->refcnt  = 2;         // caller + cache
        v->mnt     = m;
        v->ops     = ops;
        v->fs_key  = key;
        v->st_ino  = key ? key : 1;
        v->fs_priv = nullptr;

        vnode_pool[slot] = v;
        vnode_count++;
        if (out_rc) *out_rc = 0;
        return v;
    }

    void ref(vnode* v)
    {
        if (v)
            v->refcnt++;
    }

    void unref(vnode* v)
    {
        if (!v)
            return;
        if (v->refcnt == 0)     // double-unref is a kernel bug; be loud
        {
            uart::printf("vfs: unref of dead vnode %llx\n", (uint64_t)(uintptr_t)v);
            return;
        }
        if (--v->refcnt > 0)
            return;

        // Last reference (the cache's own). The FS gets one final chance to
        // write back dirty metadata, then the structures go away.
        for (uint32_t i = 0; i < MAX_VNODES; i++)
            if (vnode_pool[i] == v)
            {
                vnode_pool[i] = nullptr;
                vnode_count--;
                break;
            }

        if (v->ops->release)
            v->ops->release(v);

        if (v->fs_priv)
            kfree(v->fs_priv);
        kfree(v);
    }

    void touch(vnode* v, bool mtime_now)
    {
        v->flags |= VF_DIRTY;
        if (mtime_now)
        {
            v->mtime = now_epoch();
            v->ctime = v->mtime;
        }
    }

    // -----------------------------------------------------------------------
    // mount table
    // -----------------------------------------------------------------------

    sint64_t mount_at(vnode* point_dir, const char* devname, vfs_fs* fs, void* arg)
    {
        if (mount_cnt >= MAX_MOUNTS)
            return -ENFILE;

        vnode* root = nullptr;
        sint64_t rc = fs->mount_fs(arg, &root);
        if (rc != 0)
            return rc;

        mount* m = nullptr;
        for (uint32_t i = 0; i < MAX_MOUNTS; i++)
            if (!mounts[i].active)
            {
                m = &mounts[i];
                break;
            }
        if (!m)
        {
            unref(root);
            return -ENFILE;
        }

        m->root   = root;
        m->point  = point_dir;
        m->active = true;
        strncpy(m->devname, devname, sizeof(m->devname) - 1);

        // The FS root's mnt is the new mount (mount_fs cannot know it).
        root->mnt = m;
        mount_cnt++;

        uart::printf("vfs: %s mounted on %s\n", devname,
                     point_dir ? "mount point" : "/");
        return 0;
    }

    // The mount whose point is `dir` (a directory being stepped into).
    static mount* mount_over(vnode* dir)
    {
        for (uint32_t i = 0; i < MAX_MOUNTS; i++)
            if (mounts[i].active && mounts[i].point == dir)
                return &mounts[i];
        return nullptr;
    }

    // The mount that `v` belongs to (for crossing .. out of a mounted FS).
    static mount* mount_of(vnode* v)
    {
        for (uint32_t i = 0; i < MAX_MOUNTS; i++)
            if (mounts[i].active && mounts[i].root == v)
                return &mounts[i];
        return v->mnt;
    }

    sint64_t umount(mount* m)
    {
        if (!m || !m->active)
            return -EINVAL;

        // Flush everything this FS wrote.
        if (m->root->ops->fsync)
            m->root->ops->fsync(m->root);

        // Drop every cached vnode of this FS. Each holds the cache's single
        // reference; release() gives the FS a chance to write back dirty
        // metadata before the structures disappear.
        // (3.7 will refuse the umount while a process still holds an fd.)
        for (uint32_t i = 0; i < MAX_VNODES; i++)
        {
            vnode* v = vnode_pool[i];
            if (!v || v->mnt != m)
                continue;
            vnode_pool[i] = nullptr;
            vnode_count--;
            if (v->ops->release)
                v->ops->release(v);
            if (v->fs_priv)
                kfree(v->fs_priv);
            kfree(v);
        }

        m->active = false;
        mount_cnt--;

        vnode* point = m->point;
        m->root = nullptr;      // its vnode was freed with the pool sweep
        m->point = nullptr;

        if (point)
            unref(point);

        bcache::flush(nullptr);
        return 0;
    }

    mount* root_mount()
    {
        for (uint32_t i = 0; i < MAX_MOUNTS; i++)
            if (mounts[i].active && mounts[i].point == nullptr)
                return &mounts[i];
        return nullptr;
    }

    mount* mount_count_get(uint32_t i)
    {
        uint32_t n = 0;
        for (uint32_t k = 0; k < MAX_MOUNTS; k++)
            if (mounts[k].active)
            {
                if (n == i)
                    return &mounts[k];
                n++;
            }
        return nullptr;
    }

    uint32_t mount_count()
    {
        return mount_cnt;
    }

    // -----------------------------------------------------------------------
    // namei
    // -----------------------------------------------------------------------

    // Step one component. `cur` is referenced on entry; on success it is
    // released and *out is referenced in its place. On failure *out is not
    // set and cur keeps its reference.
    static sint64_t step(vnode* cur, const char* comp, uint32_t clen,
                         vnode** out)
    {
        if (clen > NAME_MAX)
            return -ENAMETOOLONG;

        char name[NAME_MAX + 1];
        for (uint32_t i = 0; i < clen; i++)
            name[i] = comp[i];
        name[clen] = '\0';

        if (clen == 1 && name[0] == '.')
        {
            *out = cur;
            return 0;
        }

        if (clen == 2 && name[0] == '.' && name[1] == '.')
        {
            // ".." from a mounted FS's root leaves the mount: the effective
            // parent is the mount point's parent. getparent() at an FS root
            // returns the root itself (contract), so "/" stays "/".
            if (cur == cur->mnt->root && cur->mnt->point)
            {
                vnode* point = cur->mnt->point;
                ref(point);
                unref(cur);
                cur = point;
            }

            vnode* parent = nullptr;
            char pname[NAME_MAX + 1];
            sint64_t rc = cur->ops->getparent(cur, &parent, pname);
            if (rc != 0)
                return rc;
            unref(cur);
            *out = parent;
            return 0;
        }

        if (cur->type != vtype::DIR)
            return -ENOTDIR;

        vnode* child = nullptr;
        sint64_t rc = cur->ops->lookup(cur, name, &child);
        if (rc != 0)
            return rc;

        // If the child is a directory with a mount over it, descend.
        if (child->type == vtype::DIR)
        {
            mount* over = mount_over(child);
            if (over)
            {
                unref(child);
                ref(over->root);
                child = over->root;
            }
        }

        unref(cur);
        *out = child;
        return 0;
    }

    sint64_t lookup(const char* path, vnode* cwd, vnode** out, bool must_be_dir)
    {
        if (!path || !path[0])
            return -ENOENT;

        // Copy the path into a heap buffer: PATH_MAX on the kernel stack is
        // not an option, and the walk below needs a mutable scratch copy.
        uint32_t len = 0;
        while (path[len])
            len++;
        if (len >= PATH_MAX)
            return -ENAMETOOLONG;

        char* p = (char*)kmalloc(len + 1);
        if (!p)
            return -ENOMEM;
        strncpy(p, path, len + 1);

        vnode* cur;
        if (p[0] == '/')
        {
            mount* rm = root_mount();
            if (!rm)
            {
                kfree(p);
                return -ENOENT;
            }
            cur = rm->root;
            ref(cur);
        }
        else
        {
            if (!cwd)
            {
                kfree(p);
                return -ENOENT;
            }
            cur = cwd;
            ref(cur);
        }

        bool trailing_slash = (len > 1 && p[len - 1] == '/');

        sint64_t rc = 0;
        const char* seg = p;
        while (*seg)
        {
            // Skip duplicate slashes.
            while (*seg == '/')
                seg++;
            if (!*seg)
                break;

            const char* end = seg;
            while (*end && *end != '/')
                end++;

            uint32_t clen = (uint32_t)(end - seg);
            vnode* next = nullptr;
            rc = step(cur, seg, clen, &next);
            if (rc != 0)
                break;          // cur keeps its reference; freed below
            cur = next;         // step() released cur on success
            seg = end;
        }

        kfree(p);

        if (rc != 0)
        {
            if (cur)
                unref(cur);
            return rc;
        }

        if (must_be_dir || trailing_slash)
        {
            if (cur->type != vtype::DIR)
            {
                unref(cur);
                return -ENOTDIR;
            }
        }

        *out = cur;
        return 0;
    }

    sint64_t lookup_parent(const char* path, vnode* cwd, vnode** out_dir,
                           char* name)
    {
        if (!path || !path[0])
            return -ENOENT;

        uint32_t len = 0;
        while (path[len])
            len++;
        if (len >= PATH_MAX)
            return -ENAMETOOLONG;

        // The last component: everything before it is the parent path.
        // Strip trailing slashes first ("/dir/" has no final component).
        uint32_t end = len;
        while (end > 1 && path[end - 1] == '/')
            end--;

        uint32_t cut = end;
        while (cut > 0 && path[cut - 1] != '/')
            cut--;

        uint32_t nlen = end - cut;
        if (nlen == 0 || nlen > NAME_MAX)
            return -ENAMETOOLONG;
        for (uint32_t i = 0; i < nlen; i++)
            name[i] = path[cut + i];
        name[nlen] = '\0';

        if (nlen == len)
        {
            // No slash at all: relative to cwd, parent is cwd itself.
            if (!cwd)
                return -ENOENT;
            ref(cwd);
            *out_dir = cwd;
            return 0;
        }

        uint32_t plen = cut;
        while (plen > 1 && path[plen - 1] == '/')
            plen--;

        if (plen == 0 && path[0] != '/')
        {
            // Relative "name" or "name/": the parent is cwd itself.
            if (!cwd)
                return -ENOENT;
            ref(cwd);
            *out_dir = cwd;
            return 0;
        }

        // Parent path: everything before the final slash. "/" stays "/".
        char* pp = (char*)kmalloc(plen + 1);
        if (!pp)
            return -ENOMEM;
        for (uint32_t i = 0; i < plen; i++)
            pp[i] = path[i];
        pp[plen] = '\0';
        if (plen == 0)
        {
            pp[0] = '/';
            pp[1] = '\0';
        }

        vnode* dir = nullptr;
        sint64_t rc = lookup(pp, cwd, &dir, true);
        kfree(pp);
        if (rc != 0)
            return rc;

        *out_dir = dir;
        return 0;
    }

    sint64_t open_path(const char* path, vnode* cwd, vnode** out)
    {
        return lookup(path, cwd, out, false);
    }

    sint64_t get_path(vnode* v, char* buf, uint32_t bufsize, uint32_t* len)
    {
        if (bufsize < 2)
            return -EINVAL;

        // Build the path backwards into a heap scratch buffer (PATH_MAX on
        // the kernel stack is not an option), then copy it out front-aligned.
        char* scratch = (char*)kmalloc(PATH_MAX);
        if (!scratch)
            return -ENOMEM;

        uint32_t used = 0;                  // bytes used from the END of scratch
        vnode* cur = v;
        ref(cur);

        bool is_root = false;
        uint32_t guard = 0;

        while (!is_root && guard++ < PATH_MAX / 2)
        {
            mount* m = mount_of(cur);

            // At a mounted FS root: jump to the mount point (its name comes
            // from the parent FS), unless this is /.
            if (m && m->point && cur == m->root)
            {
                unref(cur);
                cur = m->point;
                ref(cur);
                m = mount_of(cur);
            }

            if (m && !m->point && cur == m->root)
            {
                is_root = true;               // reached "/"
                break;
            }

            vnode* parent = nullptr;
            char name[NAME_MAX + 1];
            sint64_t rc = cur->ops->getparent(cur, &parent, name);
            if (rc != 0)
            {
                unref(cur);
                kfree(scratch);
                return rc;
            }
            if (parent == cur)
            {
                unref(cur);
                is_root = true;
                break;
            }

            uint32_t nlen = strlen(name);
            if (used + nlen + 1 >= PATH_MAX)
            {
                unref(cur);
                unref(parent);
                kfree(scratch);
                return -ENAMETOOLONG;
            }
            // Prepend "/name".
            used += nlen;
            for (uint32_t i = 0; i < nlen; i++)
                scratch[PATH_MAX - used + i] = name[i];
            used += 1;
            scratch[PATH_MAX - used] = '/';

            unref(cur);
            cur = parent;
        }

        unref(cur);

        if (used == 0)
        {
            buf[0] = '/';
            buf[1] = '\0';
            if (len) *len = 1;
            kfree(scratch);
            return 0;
        }

        if (used + 1 > bufsize)
        {
            kfree(scratch);
            return -ERANGE;
        }
        for (uint32_t i = 0; i < used; i++)
            buf[i] = scratch[PATH_MAX - used + i];
        buf[used] = '\0';
        if (len) *len = used;

        kfree(scratch);
        return 0;
    }

    sint64_t sync_all()
    {
        sint64_t first_err = 0;
        for (uint32_t i = 0; i < MAX_MOUNTS; i++)
        {
            if (!mounts[i].active)
                continue;
            vnode* root = mounts[i].root;
            if (root->ops->fsync)
            {
                sint64_t rc = root->ops->fsync(root);
                if (rc != 0 && first_err == 0)
                    first_err = rc;
            }
        }
        sint64_t rc = bcache::flush(nullptr);
        if (rc != 0 && first_err == 0)
            first_err = rc;
        return first_err;
    }
}
