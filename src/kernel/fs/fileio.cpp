// Files opened, read and written for a program (sffile.cpp, the SDK's
// SfFiles and SfFile): the handle-table and bounce-buffer part below the
// vnode layer. Results are >= 0, or -errno.
//
// Big transfers go through a 64 KiB bounce buffer in chunks: file data is
// never kmalloc'ed whole, and short reads/writes are legal.

#include "../../include/fs/fileio.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/process.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/stdlib/string.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/fcntl.h"
#include "../../sdk/include/abi/stat.h"

namespace
{
    const uint64_t BOUNCE_SIZE = 64 * 1024;

    // Scratch for the bounce copies: only one call runs in the kernel at a
    // time (the big kernel lock), so a single static buffer is safe.
    uint8_t bounce[BOUNCE_SIZE];

    sint64_t open_target(vnode* target, sint32_t flags, bool created);

    // Open `path` below `base`: the new handle, or -errno. lflags: vfs LOOKUP_*.
    sint64_t do_openat(vnode* base, const char* path, sint32_t flags,
                       uint32_t mode, uint32_t lflags = 0)
    {
        vnode* target = nullptr;
        bool created = false;

        if (flags & O_CREAT)
        {
            vnode* parent = nullptr;
            char name[NAME_MAX + 1];
            sint64_t rc = vfs::lookup_parent(path, base, &parent, name, lflags);
            if (rc != 0)
                return rc;

            // A trailing-slash create ("dir/", O_CREAT) is -EISDIR.
            uint32_t plen = strlen(path);
            if (plen > 1 && path[plen - 1] == '/')
            {
                vfs::unref(parent);
                return -EISDIR;
            }

            rc = parent->ops->lookup(parent, name, &target);
            if (rc == 0)
            {
                vfs::unref(parent);
                if (flags & O_EXCL)
                {
                    vfs::unref(target);
                    return -EEXIST;
                }
            }
            else
            {
                if (rc != -ENOENT)
                {
                    vfs::unref(parent);
                    return rc;
                }
                if (parent->type != vtype::DIR)
                {
                    vfs::unref(parent);
                    return -ENOTDIR;
                }
                rc = parent->ops->create(parent, name, mode & 0777, &target);
                vfs::unref(parent);
                if (rc != 0)
                    return rc;
                created = true;
            }

            if (flags & O_DIRECTORY)
            {
                if (target->type != vtype::DIR)
                {
                    vfs::unref(target);
                    return -ENOTDIR;
                }
            }
        }
        else
        {
            bool must_dir = (flags & O_DIRECTORY) != 0;
            sint64_t rc = vfs::lookup(path, base, &target, must_dir, lflags);
            if (rc != 0)
                return rc;
        }

        return open_target(target, flags, created);
    }

    // The rest of an open once the target is found: checks, truncation,
    // the file and its handle. Takes over the reference to `target`.
    sint64_t open_target(vnode* target, sint32_t flags, bool created)
    {
        uint32_t acc = (uint32_t)flags & O_ACCMODE;
        if (target->type == vtype::DIR && acc != O_RDONLY)
        {
            vfs::unref(target);
            return -EISDIR;
        }
        // Writing to a read-only file (FAT READ_ONLY) is EACCES.
        if (acc != O_RDONLY && target->type == vtype::REG &&
            !(target->mode & S_IWUSR))
        {
            vfs::unref(target);
            return -EACCES;
        }

        if ((flags & O_TRUNC) && !created && target->type == vtype::REG &&
            acc != O_RDONLY)
        {
            sint64_t rc = target->ops->truncate(target, 0);
            if (rc != 0)
            {
                vfs::unref(target);
                return rc;
            }
        }

        file* f = filesys::file_open(target, (uint32_t)flags);
        if (!f)
            return -ENFILE;

        if (flags & O_APPEND)
        {
            struct stat st;
            target->ops->getattr(target, &st);
            f->offset = (uint64_t)st.st_size;
        }

        sint32_t fd = -1;
        sint64_t rc = filesys::fd_alloc(process::cur_handles(), f,
                                             (flags & O_CLOEXEC) != 0, &fd);
        if (rc != 0)
            return rc;
        return fd;
    }

    // Core read at an explicit offset (offset==nullptr: use and advance the
    // file offset). Returns the count or -errno.
    sint64_t do_read(file* f, uint64_t* offset, uint64_t user_buf,
                     uint64_t count)
    {
        if ((f->flags & O_ACCMODE) == O_WRONLY)
            return -EBADF;      // fd not open for reading

        if (f->vn->type == vtype::DIR)
            return -EISDIR;

        if (count == 0)
            return 0;
        if (!uaccess::writable(user_buf, count))
            return -EFAULT;

        uint64_t total = 0;
        uint64_t off = offset ? *offset : f->offset;

        while (total < count)
        {
            uint64_t chunk = count - total;
            if (chunk > BOUNCE_SIZE)
                chunk = BOUNCE_SIZE;

            uint64_t done = 0;
            sint64_t rc = f->vn->ops->read(f->vn, off, bounce, chunk, &done);
            if (rc != 0)
                return total ? (sint64_t)total : rc;
            if (done == 0)
                break;                    // EOF

            if (!uaccess::copy_to_user(user_buf + total, bounce, done))
                return total ? (sint64_t)total : -EFAULT;

            total += done;
            off   += done;
            if (done < chunk)
                break;                    // short read: the end of the file
        }

        if (!offset)
            f->offset = off;
        else
            *offset = off;
        return (sint64_t)total;
    }

    sint64_t do_write(file* f, uint64_t* offset, uint64_t user_buf,
                      uint64_t count)
    {
        if ((f->flags & O_ACCMODE) == O_RDONLY)
            return -EBADF;

        if (f->vn->type == vtype::DIR)
            return -EISDIR;

        if (count == 0)
            return 0;
        if (!uaccess::readable(user_buf, count))
            return -EFAULT;

        uint64_t total = 0;
        uint64_t off = (f->flags & O_APPEND)
                     ? (uint64_t)-1      // resolved per chunk below
                     : (offset ? *offset : f->offset);

        while (total < count)
        {
            uint64_t chunk = count - total;
            if (chunk > BOUNCE_SIZE)
                chunk = BOUNCE_SIZE;

            if (!uaccess::copy_from_user(bounce, user_buf + total, chunk))
                return total ? (sint64_t)total : -EFAULT;

            uint64_t woff = off;
            if (f->flags & O_APPEND)
            {
                // O_APPEND: every chunk lands after the current end, atomic
                // within the syscall (nothing else runs mid-syscall).
                struct stat st;
                f->vn->ops->getattr(f->vn, &st);
                woff = (uint64_t)st.st_size;
            }

            uint64_t done = 0;
            sint64_t rc = f->vn->ops->write(f->vn, woff, bounce, chunk, &done);
            if (rc != 0)
                return total ? (sint64_t)total : rc;
            if (done == 0)
                return total ? (sint64_t)total : -EIO;

            total += done;
            if (!(f->flags & O_APPEND))
            {
                off += done;
                if (offset)
                    *offset = off;
                else
                    f->offset = off;
            }
            else if (offset)
                *offset = woff + done;
            else
                f->offset = woff + done;
        }
        return (sint64_t)total;
    }
}

namespace fileio
{
    sint64_t open_at(vnode* base, const char* path, sint32_t flags, uint32_t mode,
                     uint32_t lflags)
    {
        return do_openat(base, path, flags, mode, lflags);
    }

    sint64_t open_vnode(vnode* v, sint32_t flags)
    {
        vfs::ref(v);
        return open_target(v, flags, false);
    }

    sint64_t read(file* f, uint64_t user_buf, uint64_t count)
    {
        return do_read(f, nullptr, user_buf, count);
    }

    sint64_t write(file* f, uint64_t user_buf, uint64_t count)
    {
        return do_write(f, nullptr, user_buf, count);
    }
}
