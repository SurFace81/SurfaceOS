// Mounts of other volumes under /mount. See mounts.h.

#include "../../include/fs/mounts.h"
#include "../../include/fs/fat32fs.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    // Write /mount's volume through after a mount point came or went:
    // nothing else flushes it (no file of it was written), and the
    // free-cluster count on disk would stay behind.
    void flush_mount_dir(vnode* dir)
    {
        if (dir->ops->fsync)
            dir->ops->fsync(dir);
    }

    // Remove the empty directory /mount/<name>. 0 or -errno.
    sint64_t remove_mount_dir(const char* name)
    {
        vnode* dir = nullptr;
        sint64_t rc = vfs::lookup("/mount", nullptr, &dir, true);
        if (rc != 0)
            return rc;
        rc = dir->ops->rmdir ? dir->ops->rmdir(dir, name) : -EPERM;
        if (rc == 0)
            flush_mount_dir(dir);
        vfs::unref(dir);
        return rc;
    }
}

namespace mounts
{
    mount* of_device(const char* name)
    {
        for (uint32_t i = 0; vfs::mount_count_get(i); i++)
        {
            mount* m = vfs::mount_count_get(i);
            if (!m->detached && strcmp(m->devname, name) == 0)
                return m;
        }
        return nullptr;
    }

    bool has_partitions(blkdev* disk)
    {
        for (uint32_t i = 0; block::get(i); i++)
            if (block::get(i)->parent == disk)
                return true;
        return false;
    }

    sint64_t mount_device(blkdev* d)
    {
        vnode* dir = nullptr;
        sint64_t rc = vfs::lookup("/mount", nullptr, &dir, true);
        if (rc != 0)
            return rc;

        bool made = false;
        vnode* point = nullptr;
        rc = vfs::lookup(d->name, dir, &point, true);
        if (rc == -ENOENT && dir->ops->mkdir)
        {
            rc = dir->ops->mkdir(dir, d->name, 0755);
            if (rc == 0)
            {
                flush_mount_dir(dir);
                made = true;
                rc = vfs::lookup(d->name, dir, &point, true);
            }
        }
        vfs::unref(dir);
        if (rc != 0)
            return rc;

        rc = vfs::mount_at(point, d->name, &fat32fs::fs, d);   // takes the ref
        if (rc != 0 && made)
            remove_mount_dir(d->name);
        return rc;
    }

    sint64_t unmount(mount* m)
    {
        if (!m->point)
            return -EPERM;              // the root file system

        char name[sizeof(m->devname)];
        strncpy(name, m->devname, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        // Only mounts made by mount_device own their directory.
        char path[PATH_MAX];
        bool in_mount_dir = vfs::get_path(m->point, path, sizeof(path), nullptr) == 0 &&
                            strncmp(path, "/mount/", 7) == 0 &&
                            strcmp(path + 7, name) == 0;

        vnode* root = m->root;
        sint64_t rc = root->ops->fsync ? root->ops->fsync(root) : 0;
        if (rc == 0)
            rc = vfs::umount(m);
        if (rc == 0 && in_mount_dir)
            remove_mount_dir(name);
        return rc;
    }

    sint64_t prepare_power_off()
    {
        sint64_t rc = vfs::sync_all();
        mount* root = vfs::root_mount();
        if (root)
        {
            vfs::set_cwd(nullptr);
            sint64_t urc = vfs::umount(root, true);
            if (rc == 0)
                rc = urc;
        }
        return rc;
    }
}
