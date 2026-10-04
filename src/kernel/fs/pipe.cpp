// Pipes. See pipe.h.

#include "../../include/fs/pipe.h"
#include "../../include/fs/vfs.h"
#include "../../include/cpu/wait.h"
#include "../../include/mm/heap.h"
#include "../../include/stdlib/string.h"
#include "../../include/errno.h"

namespace
{
    const uint64_t PIPE_SIZE = 16 * 1024;

    // What both ends share: a ring of PIPE_SIZE bytes, `count` of them
    // waiting from `head` on. Readers and writers sleep on wq.
    struct pipe_buf
    {
        uint8_t    data[PIPE_SIZE];
        uint64_t   head;
        uint64_t   count;
        bool       reader_gone;
        bool       writer_gone;
        wait_queue wq;
    };

    // fs_key tells the ends apart.
    const uint64_t READ_END  = 0;
    const uint64_t WRITE_END = 1;

    pipe_buf* of(vnode* v)
    {
        return (pipe_buf*)v->fs_priv;
    }

    bool readable(void* arg)
    {
        pipe_buf* p = (pipe_buf*)arg;
        return p->count || p->writer_gone;
    }

    bool writable(void* arg)
    {
        pipe_buf* p = (pipe_buf*)arg;
        return p->count < PIPE_SIZE || p->reader_gone;
    }

    sint64_t pipe_read(vnode* v, uint64_t, void* buf, uint64_t len, uint64_t* done)
    {
        *done = 0;
        pipe_buf* p = of(v);
        if (v->fs_key != READ_END)
            return -EBADF;
        if (!wait::wait_event(&p->wq, readable, p, 0))
            return -EINTR;
        uint8_t* out = (uint8_t*)buf;
        uint64_t n = len < p->count ? len : p->count;
        for (uint64_t i = 0; i < n; i++)
            out[i] = p->data[(p->head + i) % PIPE_SIZE];
        p->head = (p->head + n) % PIPE_SIZE;
        p->count -= n;
        *done = n;
        wait::wake_up(&p->wq);
        return 0;
    }

    // All of it, a piece at a time as the reader makes room.
    sint64_t pipe_write(vnode* v, uint64_t, const void* buf, uint64_t len, uint64_t* done)
    {
        *done = 0;
        pipe_buf* p = of(v);
        if (v->fs_key != WRITE_END)
            return -EBADF;
        const uint8_t* in = (const uint8_t*)buf;
        while (*done < len)
        {
            if (!wait::wait_event(&p->wq, writable, p, 0))
                return -EINTR;
            if (p->reader_gone)
                return -EPIPE;
            uint64_t room = PIPE_SIZE - p->count;
            uint64_t n = len - *done < room ? len - *done : room;
            for (uint64_t i = 0; i < n; i++)
                p->data[(p->head + p->count + i) % PIPE_SIZE] = in[*done + i];
            p->count += n;
            *done += n;
            wait::wake_up(&p->wq);
        }
        return 0;
    }

    sint64_t pipe_getattr(vnode*, struct stat* st)
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFIFO | 0600;
        return 0;
    }

    // An end is gone: the other one hears of it, and the last frees the
    // buffer (vfs::unref must not: both ends point at it).
    void pipe_release(vnode* v)
    {
        pipe_buf* p = of(v);
        v->fs_priv = nullptr;
        bool other_gone = v->fs_key == READ_END ? p->writer_gone : p->reader_gone;
        if (other_gone)
        {
            kfree(p);
            return;
        }
        if (v->fs_key == READ_END)
            p->reader_gone = true;
        else
            p->writer_gone = true;
        wait::wake_up(&p->wq);
    }

    vnode_ops pipe_ops =
    {
        nullptr, nullptr, nullptr, nullptr, nullptr,    // no names in a pipe
        nullptr, nullptr,
        pipe_read, pipe_write,
        nullptr,            // truncate
        nullptr,            // readdir
        pipe_getattr,
        nullptr, nullptr, nullptr, nullptr,             // setattr, fsync, ioctl, poll_ready
        pipe_release,
    };

    vnode* new_end(pipe_buf* p, uint64_t end)
    {
        vnode* v = (vnode*)kmalloc(sizeof(vnode));
        if (!v)
            return nullptr;
        memset(v, 0, sizeof(*v));
        v->type    = vtype::PIPE;
        v->refcnt  = 1;
        v->mode    = S_IFIFO | 0600;
        v->ops     = &pipe_ops;
        v->fs_key  = end;
        v->fs_priv = p;
        return v;
    }
}

namespace pipe
{
    sint64_t create(vnode** rd, vnode** wr)
    {
        pipe_buf* p = (pipe_buf*)kmalloc(sizeof(pipe_buf));
        if (!p)
            return -ENOMEM;
        memset(p, 0, sizeof(*p));
        *rd = new_end(p, READ_END);
        *wr = *rd ? new_end(p, WRITE_END) : nullptr;
        if (!*wr)
        {
            if (*rd)
                kfree(*rd);
            kfree(p);
            return -ENOMEM;
        }
        return 0;
    }
}
