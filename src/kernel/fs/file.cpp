// Open files and their handles. See file.h.
//
// The file pool is a static array: MAX_FILES open files at once, which with
// 64 handles per process is generous. Allocation
// is a linear scan - at this size it beats any free-list bookkeeping.

#include "../../include/fs/file.h"
#include "../../include/fs/vfs.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
#include "../../include/errno.h"
#include "../../include/fs/openflags.h"

namespace
{
    const uint32_t MAX_FILES = 256;

    file pool[MAX_FILES];
    uint32_t next_id = 1;

    // The last reference is gone: close the description.
    void file_destroy(kobject* o)
    {
        file* f = (file*)o;

        // The close of a file opened for writing: its data must survive a
        // yanked stick, so the last close flushes through to the device
        // (dirty vnode entry + FSInfo + bcache + SYNCHRONIZE CACHE).
        uint32_t acc = f->flags & O_ACCMODE;
        if ((acc == O_WRONLY || acc == O_RDWR) &&
            f->vn->ops->fsync)
            f->vn->ops->fsync(f->vn);

        vfs::unref(f->vn);
        f->vn = nullptr;
        f->used = false;
    }

    const kobject_ops file_ops =
    {
        obj_type::File, "file", file_destroy,
        nullptr, nullptr, nullptr,      // not waitable
    };
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
            kobj::init(&f->hdr, &file_ops);
            f->vn     = vn;             // takes the caller's reference
            f->offset = 0;
            f->flags  = flags & (O_ACCMODE | O_APPEND);
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
            kobj::get(&f->hdr);
    }

    void file_put(file* f)
    {
        if (f)
            kobj::put(&f->hdr);
    }

    // -----------------------------------------------------------------------
    // descriptors
    // -----------------------------------------------------------------------

    sint64_t fd_alloc(handle_table* t, file* f, sint32_t* out_fd)
    {
        sint64_t rc = handles::install(t, &f->hdr, out_fd);
        file_put(f);                    // the table holds its own now
        return rc;
    }

    file* fd_get(handle_table* t, sint32_t fd, sint64_t* rc)
    {
        sint64_t r = 0;
        kobject* o = handles::get(t, fd, obj_type::File, &r);
        if (rc)
            *rc = r;
        return (file*)o;
    }

    sint64_t fd_close(handle_table* t, sint32_t fd)
    {
        return handles::close(t, fd);
    }

}
