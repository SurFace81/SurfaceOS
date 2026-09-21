// Open files and descriptor tables (stage 3.4). See file.h.
//
// The file pool is a static array: MAX_FILES open descriptions at once,
// which with 64 fds per process and 32 processes is generous. Allocation
// is a linear scan - at this size it beats any free-list bookkeeping.

#include "../../include/fs/file.h"
#include "../../include/fs/vfs.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/fcntl.h"

namespace
{
    const uint32_t MAX_FILES = 256;

    file pool[MAX_FILES];
    uint32_t next_id = 1;
}

namespace filesys
{
    void init()
    {
        memory::memset((uint8_t*)pool, 0, sizeof(pool));
        next_id = 1;
    }

    // -----------------------------------------------------------------------
    // open file descriptions
    // -----------------------------------------------------------------------

    file* file_open(vnode* vn, uint32_t flags)
    {
        for (uint32_t i = 0; i < MAX_FILES; i++)
        {
            if (pool[i].used)
                continue;

            file* f = &pool[i];
            f->vn     = vn;             // takes the caller's reference
            f->offset = 0;
            f->flags  = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
            f->refcnt = 1;
            f->id     = next_id++;
            f->used   = true;
            return f;
        }
        // Out of descriptions: give the vnode reference back.
        vfs::unref(vn);
        return nullptr;
    }

    bool any_open_on(const mount* m)
    {
        for (uint32_t i = 0; i < MAX_FILES; i++)
            if (pool[i].used && pool[i].vn && pool[i].vn->mnt == m)
                return true;
        return false;
    }

    void file_get(file* f)
    {
        if (f)
            f->refcnt++;
    }

    void file_put(file* f)
    {
        if (!f)
            return;
        if (--f->refcnt > 0)
            return;

        f->used = false;

        // POSIX close of a file opened for writing: its data must survive a
        // yanked stick, so the last close flushes through to the device
        // (dirty vnode entry + FSInfo + bcache + SYNCHRONIZE CACHE).
        uint32_t acc = f->flags & O_ACCMODE;
        if ((acc == O_WRONLY || acc == O_RDWR) &&
            f->vn->ops->fsync)
            f->vn->ops->fsync(f->vn);

        vfs::unref(f->vn);
        f->vn = nullptr;
    }

    // -----------------------------------------------------------------------
    // fd tables
    // -----------------------------------------------------------------------

    void fdtable_init(fd_table* t)
    {
        memory::memset((uint8_t*)t, 0, sizeof(fd_table));
    }

    sint64_t fdtable_alloc(fd_table* t, file* f, bool cloexec, sint32_t* out_fd)
    {
        for (sint32_t i = 0; i < FD_TABLE_SIZE; i++)
        {
            if (t->slots[i].f)
                continue;
            t->slots[i].f = f;
            t->slots[i].cloexec = cloexec ? 1 : 0;
            *out_fd = i;
            return 0;
        }
        file_put(f);
        return -EMFILE;
    }

    file* fdtable_get(fd_table* t, sint32_t fd, sint64_t* rc)
    {
        if (fd < 0 || fd >= FD_TABLE_SIZE || !t->slots[fd].f)
        {
            if (rc) *rc = -EBADF;
            return nullptr;
        }
        if (rc) *rc = 0;
        return t->slots[fd].f;
    }

    sint64_t fdtable_close(fd_table* t, sint32_t fd)
    {
        if (fd < 0 || fd >= FD_TABLE_SIZE || !t->slots[fd].f)
            return -EBADF;

        file_put(t->slots[fd].f);
        t->slots[fd].f = nullptr;
        t->slots[fd].cloexec = 0;
        return 0;
    }

    sint32_t fdtable_lowest_free(const fd_table* t, sint32_t minfd)
    {
        if (minfd < 0)
            minfd = 0;
        for (sint32_t i = minfd; i < FD_TABLE_SIZE; i++)
            if (!t->slots[i].f)
                return i;
        return -1;
    }

    sint64_t fdtable_dup(fd_table* t, sint32_t oldfd, sint32_t newfd,
                         bool cloexec, bool explicit_new, sint32_t* out_fd)
    {
        if (oldfd < 0 || oldfd >= FD_TABLE_SIZE || !t->slots[oldfd].f)
            return -EBADF;

        if (explicit_new)
        {
            if (newfd < 0 || newfd >= FD_TABLE_SIZE)
                return -EBADF;
            if (newfd == oldfd)
            {
                // dup3 rejects old == new; dup2 makes it a no-op that still
                // reports the fd. Report it through *out_fd like every other
                // success, so callers never have to special-case the rc.
                if (cloexec)
                    return -EINVAL;
                *out_fd = oldfd;
                return 0;
            }
        }
        else
        {
            newfd = -1;
            for (sint32_t i = 0; i < FD_TABLE_SIZE; i++)
                if (!t->slots[i].f)
                {
                    newfd = i;
                    break;
                }
            if (newfd < 0)
                return -EMFILE;
        }

        file_put(t->slots[newfd].f);        // dup2 closes the target (null ok)
        t->slots[newfd].f = t->slots[oldfd].f;
        file_get(t->slots[newfd].f);
        t->slots[newfd].cloexec = cloexec ? 1 : 0;

        *out_fd = newfd;
        return 0;
    }

    sint64_t fdtable_getfd(fd_table* t, sint32_t fd)
    {
        if (fd < 0 || fd >= FD_TABLE_SIZE || !t->slots[fd].f)
            return -EBADF;
        return t->slots[fd].cloexec ? FD_CLOEXEC : 0;
    }

    sint64_t fdtable_setfd(fd_table* t, sint32_t fd, sint64_t arg)
    {
        if (fd < 0 || fd >= FD_TABLE_SIZE || !t->slots[fd].f)
            return -EBADF;
        if (arg & ~(sint64_t)FD_CLOEXEC)
            return -EINVAL;
        t->slots[fd].cloexec = (arg & FD_CLOEXEC) ? 1 : 0;
        return 0;
    }

    void fdtable_fork(fd_table* dst, const fd_table* src)
    {
        for (sint32_t i = 0; i < FD_TABLE_SIZE; i++)
        {
            dst->slots[i].f = src->slots[i].f;
            dst->slots[i].cloexec = src->slots[i].cloexec;
            if (dst->slots[i].f)
                file_get(dst->slots[i].f);
        }
    }

    void fdtable_cloexec(fd_table* t)
    {
        for (sint32_t i = 0; i < FD_TABLE_SIZE; i++)
            if (t->slots[i].f && t->slots[i].cloexec)
            {
                file_put(t->slots[i].f);
                t->slots[i].f = nullptr;
                t->slots[i].cloexec = 0;
            }
    }

    void fdtable_close_all(fd_table* t)
    {
        for (sint32_t i = 0; i < FD_TABLE_SIZE; i++)
        {
            if (t->slots[i].f)
                file_put(t->slots[i].f);
            t->slots[i].f = nullptr;
            t->slots[i].cloexec = 0;
        }
    }
}
