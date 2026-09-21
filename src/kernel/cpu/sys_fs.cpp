// File-descriptor syscalls (stage 3.6): Linux x86_64 numbers, -errno
// results, every user pointer through uaccess.
//
// Structure of every handler:
//   1. validate scalars and pointers (uaccess: all-or-nothing up front);
//   2. resolve fds through the current process's fd table;
//   3. call the vnode layer;
//   4. copy results out through uaccess.
//
// Blocking: a tty read with no line ready returns -EAGAIN from the vnode;
// the handler then calls process::block_on_input, which rewinds RIP and
// re-executes the whole syscall on wake. Nothing touched the fd state
// before that point (the offset moves only after a successful read), so
// the restart is side-effect free.
//
// Big transfers go through a 64 KiB bounce buffer in chunks: file data is
// never kmalloc'ed whole, and short reads/writes are legal.

#include "../../include/cpu/sys_fs.h"
#include "../../include/cpu/syscall.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/process.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/fs/devfs.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/fcntl.h"
#include "../../sdk/include/abi/stat.h"
#include "../../sdk/include/abi/dirent.h"
#include "../../sdk/include/abi/termios.h"

typedef user_regs syscall_regs;

namespace
{
    const uint64_t BOUNCE_SIZE = 64 * 1024;

    // access() mode bits (Linux asm-generic values).
    const sint32_t F_OK = 0;
    const sint32_t X_OK = 1;
    const sint32_t W_OK = 2;
    const sint32_t R_OK = 4;

    // linux_dirent64.d_name offset (== sizeof without the flexible array).
    uint32_t offsetof_dirent()
    {
        return (uint32_t)((uintptr_t)((linux_dirent64*)0)->d_name);
    }

    // Per-handler scratch for the bounce copies. Only one syscall runs at a
    // time (the kernel is never preempted inside its own code), so a single
    // static buffer is safe and beats a kmalloc per call.
    uint8_t bounce[BOUNCE_SIZE];

    inline sint64_t ERR(int e)
    {
        return -(sint64_t)e;
    }

    inline void set(syscall_regs* regs, sint64_t v)
    {
        regs->rax = (uint64_t)v;
    }

    void do_rename(vnode* old_dir, uint64_t oldp, vnode* new_dir, uint64_t newp,
                   uint32_t flags, syscall_regs* regs);

    // -----------------------------------------------------------------------
    // Path fetching
    // -----------------------------------------------------------------------

    // A path from user space into a heap buffer. Returns the buffer (caller
    // kfree's it) or nullptr with regs->rax already set to -errno.
    char* fetch_path(uint64_t user_ptr, syscall_regs* regs)
    {
        if (user_ptr == 0)
        {
            set(regs, ERR(EFAULT));
            return nullptr;
        }

        // Probe the length first (uaccess::strncpy_from_user is bounded).
        char* buf = (char*)kmalloc(PATH_MAX);
        if (!buf)
        {
            set(regs, ERR(ENOMEM));
            return nullptr;
        }

        sint64_t r = uaccess::strncpy_from_user(buf, user_ptr, PATH_MAX);
        if (r == -1)
        {
            kfree(buf);
            set(regs, ERR(EFAULT));
            return nullptr;
        }
        if (r == -2)
        {
            kfree(buf);
            set(regs, ERR(ENAMETOOLONG));
            return nullptr;
        }
        if (r == 0)
        {
            kfree(buf);
            set(regs, ERR(ENOENT));
            return nullptr;
        }
        return buf;
    }

    // Resolve a dirfd argument: AT_FDCWD -> the process cwd (borrowed, not
    // referenced - it lives for the syscall), a real fd -> its vnode (also
    // borrowed: the table owns the reference for the call's duration).
    vnode* resolve_dirfd(sint32_t dirfd, syscall_regs* regs)
    {
        if (dirfd == AT_FDCWD)
            return process::cur_cwd();

        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), dirfd, &rc);
        if (!f)
        {
            set(regs, rc);
            return nullptr;
        }
        if (f->vn->type != vtype::DIR)
        {
            set(regs, ERR(ENOTDIR));
            return nullptr;
        }
        return f->vn;
    }

    // -----------------------------------------------------------------------
    // open / creat / close
    // -----------------------------------------------------------------------

    // Core of open(2) and openat(2).
    sint64_t do_openat(vnode* base, const char* path, sint32_t flags,
                       uint32_t mode)
    {
        vnode* target = nullptr;
        bool created = false;

        if (flags & O_CREAT)
        {
            vnode* parent = nullptr;
            char name[NAME_MAX + 1];
            sint64_t rc = vfs::lookup_parent(path, base, &parent, name);
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
                mode &= ~process::cur_umask();
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
            sint64_t rc = vfs::lookup(path, base, &target, must_dir);
            if (rc != 0)
                return rc;
        }

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
        sint64_t rc = filesys::fdtable_alloc(process::cur_fds(), f,
                                             (flags & O_CLOEXEC) != 0, &fd);
        if (rc != 0)
            return rc;
        return fd;
    }

    void sys_openat(syscall_regs* regs, iret_frame*)
    {
        sint32_t dirfd = (sint32_t)regs->rdi;
        sint32_t flags = (sint32_t)regs->rdx;
        uint32_t mode = (uint32_t)regs->r10;

        vnode* base = resolve_dirfd(dirfd, regs);
        if (!base)
            return;

        char* path = fetch_path(regs->rsi, regs);
        if (!path)
            return;

        sint64_t rc = do_openat(base, path, flags, mode);
        kfree(path);
        set(regs, rc);
    }

    void sys_open(syscall_regs* regs, iret_frame*)
    {
        // open(path, flags, mode) == openat(AT_FDCWD, ...)
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;

        sint64_t rc = do_openat(process::cur_cwd(), path,
                                (sint32_t)regs->rsi, (uint32_t)regs->rdx);
        kfree(path);
        set(regs, rc);
    }

    void sys_creat(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;

        sint64_t rc = do_openat(process::cur_cwd(), path,
                                O_CREAT | O_WRONLY | O_TRUNC,
                                (uint32_t)regs->rsi);
        kfree(path);
        set(regs, rc);
    }

    void sys_close(syscall_regs* regs, iret_frame*)
    {
        set(regs, filesys::fdtable_close(process::cur_fds(), (sint32_t)regs->rdi));
    }

    // -----------------------------------------------------------------------
    // read / write / pread / pwrite / readv / writev
    // -----------------------------------------------------------------------

    // Core read at an explicit offset (offset==nullptr: use and advance the
    // file offset). Returns the count or -errno; the caller handles EAGAIN.
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
            {
                if (rc == -EAGAIN && total == 0)
                    return -EAGAIN;       // caller blocks
                return total ? (sint64_t)total : rc;
            }
            if (done == 0)
                break;                    // EOF

            if (!uaccess::copy_to_user(user_buf + total, bounce, done))
                return total ? (sint64_t)total : -EFAULT;

            total += done;
            off   += done;
            if (done < chunk)
                break;                    // short read: EOF or terminal
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

    void sys_read(syscall_regs* regs, iret_frame* iret)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }

        sint64_t n = do_read(f, nullptr, regs->rsi, regs->rdx);
        if (n == -EAGAIN)
        {
            // Canonical tty read with no line ready: block and restart.
            // (No fd side effect happened, so the replay is clean.)
            process::block_on_input(regs, iret);
            return;
        }
        set(regs, n);
    }

    void sys_write(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        set(regs, do_write(f, nullptr, regs->rsi, regs->rdx));
    }

    void sys_pread(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        // The offset argument is 64-bit in one register on x86_64.
        sint64_t off = (sint64_t)regs->r10;
        if (off < 0)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (f->vn->type == vtype::DIR)
        {
            set(regs, ERR(EISDIR));
            return;
        }
        uint64_t use = (uint64_t)off;
        set(regs, do_read(f, &use, regs->rsi, regs->rdx));
    }

    void sys_pwrite(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        sint64_t off = (sint64_t)regs->r10;
        if (off < 0)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (f->flags & O_APPEND)
        {
            set(regs, ERR(EINVAL));       // pwrite on an O_APPEND fd
            return;
        }
        uint64_t use = (uint64_t)off;
        set(regs, do_write(f, &use, regs->rsi, regs->rdx));
    }

    // struct iovec (Linux layout)
    struct iovec
    {
        uint64_t iov_base;
        uint64_t iov_len;
    };

    void sys_readv(syscall_regs* regs, iret_frame* iret)
    {
        sint32_t fd = (sint32_t)regs->rdi;
        uint64_t iov_ptr = regs->rsi;
        sint32_t iovcnt = (sint32_t)regs->rdx;

        if (iovcnt <= 0 || iovcnt > 1024)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (!uaccess::readable(iov_ptr, (uint64_t)iovcnt * sizeof(iovec)))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), fd, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }

        // Sequential reads per vector element share the file offset; a tty
        // EAGAIN mid-way returns what was read so far.
        uint64_t total = 0;
        for (sint32_t i = 0; i < iovcnt; i++)
        {
            iovec v;
            if (!uaccess::copy_from_user(&v, iov_ptr + (uint64_t)i * sizeof(iovec),
                                         sizeof(v)))
            {
                set(regs, total ? (sint64_t)total : ERR(EFAULT));
                return;
            }
            if (v.iov_len == 0)
                continue;

            sint64_t n = do_read(f, nullptr, v.iov_base, v.iov_len);
            if (n == -EAGAIN)
            {
                if (total == 0)
                {
                    process::block_on_input(regs, iret);
                    return;
                }
                break;
            }
            if (n < 0)
            {
                set(regs, total ? (sint64_t)total : n);
                return;
            }
            total += (uint64_t)n;
            if ((uint64_t)n < v.iov_len)
                break;              // short read: stop like Linux
        }
        set(regs, (sint64_t)total);
    }

    void sys_writev(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = (sint32_t)regs->rdi;
        uint64_t iov_ptr = regs->rsi;
        sint32_t iovcnt = (sint32_t)regs->rdx;

        if (iovcnt <= 0 || iovcnt > 1024)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (!uaccess::readable(iov_ptr, (uint64_t)iovcnt * sizeof(iovec)))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), fd, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }

        uint64_t total = 0;
        for (sint32_t i = 0; i < iovcnt; i++)
        {
            iovec v;
            if (!uaccess::copy_from_user(&v, iov_ptr + (uint64_t)i * sizeof(iovec),
                                         sizeof(v)))
            {
                set(regs, total ? (sint64_t)total : ERR(EFAULT));
                return;
            }
            if (v.iov_len == 0)
                continue;

            sint64_t n = do_write(f, nullptr, v.iov_base, v.iov_len);
            if (n < 0)
            {
                set(regs, total ? (sint64_t)total : n);
                return;
            }
            total += (uint64_t)n;
        }
        set(regs, (sint64_t)total);
    }

    // -----------------------------------------------------------------------
    // lseek
    // -----------------------------------------------------------------------

    void sys_lseek(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }

        sint64_t off = (sint64_t)regs->rsi;
        uint32_t whence = (uint32_t)regs->rdx;

        // Character devices (tty) are not seekable.
        if (f->vn->type == vtype::CHR)
        {
            set(regs, ERR(ESPIPE));
            return;
        }

        uint64_t target;
        if (whence == SEEK_SET)
        {
            if (off < 0)
            {
                set(regs, ERR(EINVAL));
                return;
            }
            target = (uint64_t)off;
        }
        else if (whence == SEEK_CUR)
        {
            sint64_t t = (sint64_t)f->offset + off;
            if (t < 0)
            {
                set(regs, ERR(EINVAL));
                return;
            }
            target = (uint64_t)t;
        }
        else if (whence == SEEK_END)
        {
            if (f->vn->type == vtype::DIR)
            {
                // Directory stream rewind: cookie semantics, offset 0 == start.
                f->offset = 0;
                set(regs, 0);
                return;
            }
            struct stat st;
            f->vn->ops->getattr(f->vn, &st);
            sint64_t t = st.st_size + off;
            if (t < 0)
            {
                set(regs, ERR(EINVAL));
                return;
            }
            target = (uint64_t)t;
        }
        else
        {
            set(regs, ERR(EINVAL));
            return;
        }

        f->offset = target;
        set(regs, (sint64_t)target);
    }

    // -----------------------------------------------------------------------
    // dup family
    // -----------------------------------------------------------------------

    void sys_dup(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = -1;
        sint64_t rc = filesys::fdtable_dup(process::cur_fds(),
                                           (sint32_t)regs->rdi, 0,
                                           false, false, &fd);
        set(regs, rc != 0 ? rc : fd);
    }

    void sys_dup2(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = -1;
        sint64_t rc = filesys::fdtable_dup(process::cur_fds(),
                                           (sint32_t)regs->rdi,
                                           (sint32_t)regs->rsi,
                                           false, true, &fd);
        set(regs, rc != 0 ? rc : fd);
    }

    void sys_dup3(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = -1;
        sint32_t flags = (sint32_t)regs->rdx;
        if (flags & ~(sint32_t)O_CLOEXEC)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        sint64_t rc = filesys::fdtable_dup(process::cur_fds(),
                                           (sint32_t)regs->rdi,
                                           (sint32_t)regs->rsi,
                                           (flags & O_CLOEXEC) != 0, true, &fd);
        set(regs, rc != 0 ? rc : fd);
    }

    void sys_fcntl(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = (sint32_t)regs->rdi;
        sint32_t cmd = (sint32_t)regs->rsi;
        sint64_t arg = (sint64_t)regs->rdx;
        fd_table* t = process::cur_fds();

        switch (cmd)
        {
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            {
                if (fd < 0 || fd >= FD_TABLE_SIZE)
                {
                    set(regs, ERR(EBADF));
                    return;
                }
                sint32_t newfd = -1;
                sint32_t minfd = (sint32_t)arg;
                if (minfd < 0 || minfd >= FD_TABLE_SIZE)
                {
                    set(regs, ERR(EINVAL));
                    return;
                }
                file* f = nullptr;
                sint64_t rc = 0;
                f = filesys::fdtable_get(t, fd, &rc);
                if (!f)
                {
                    set(regs, rc);
                    return;
                }
                for (sint32_t i = minfd; i < FD_TABLE_SIZE; i++)
                {
                    sint64_t r2 = filesys::fdtable_dup(t, fd, i,
                                                       cmd == F_DUPFD_CLOEXEC,
                                                       true, &newfd);
                    if (r2 == 0)
                    {
                        set(regs, newfd);
                        return;
                    }
                    if (r2 != -EBADF && r2 != -EEXIST)
                    {
                        set(regs, r2);
                        return;
                    }
                }
                set(regs, ERR(EMFILE));
                return;
            }
            case F_GETFD:
                set(regs, filesys::fdtable_getfd(t, fd));
                return;
            case F_SETFD:
                set(regs, filesys::fdtable_setfd(t, fd, arg));
                return;
            case F_GETFL:
            {
                sint64_t rc = 0;
                file* f = filesys::fdtable_get(t, fd, &rc);
                if (!f)
                {
                    set(regs, rc);
                    return;
                }
                set(regs, (sint64_t)f->flags);
                return;
            }
            case F_SETFL:
            {
                sint64_t rc = 0;
                file* f = filesys::fdtable_get(t, fd, &rc);
                if (!f)
                {
                    set(regs, rc);
                    return;
                }
                // Only O_APPEND and O_NONBLOCK are changeable (Linux).
                uint32_t settable = O_APPEND | O_NONBLOCK;
                if ((uint32_t)arg & ~settable)
                {
                    set(regs, ERR(EINVAL));
                    return;
                }
                f->flags = (f->flags & ~settable) | ((uint32_t)arg & settable);
                set(regs, 0);
                return;
            }
            default:
                set(regs, ERR(EINVAL));
                return;
        }
    }

    // -----------------------------------------------------------------------
    // stat family
    // -----------------------------------------------------------------------

    void sys_stat(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        if (!uaccess::writable(regs->rsi, sizeof(struct stat)))
        {
            kfree(path);
            set(regs, ERR(EFAULT));
            return;
        }

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, process::cur_cwd(), &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        struct stat st;
        rc = v->ops->getattr(v, &st);
        vfs::unref(v);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        if (!uaccess::copy_to_user(regs->rsi, &st, sizeof(st)))
        {
            set(regs, ERR(EFAULT));
            return;
        }
        set(regs, 0);
    }

    void sys_fstat(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        if (!uaccess::writable(regs->rsi, sizeof(struct stat)))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        struct stat st;
        rc = f->vn->ops->getattr(f->vn, &st);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        if (!uaccess::copy_to_user(regs->rsi, &st, sizeof(st)))
        {
            set(regs, ERR(EFAULT));
            return;
        }
        set(regs, 0);
    }

    // lstat == stat: no symlinks.
    void sys_lstat(syscall_regs* regs, iret_frame*)
    {
        sys_stat(regs, nullptr);
    }

    void sys_newfstatat(syscall_regs* regs, iret_frame*)
    {
        sint32_t dirfd = (sint32_t)regs->rdi;
        uint64_t path_ptr = regs->rsi;
        uint64_t stat_ptr = regs->rdx;
        sint32_t flags = (sint32_t)regs->r10;

        if (!uaccess::writable(stat_ptr, sizeof(struct stat)))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        // AT_EMPTY_PATH with an empty string: fstat(dirfd).
        if (flags & AT_EMPTY_PATH)
        {
            char probe[2];
            sint64_t r = uaccess::strncpy_from_user(probe, path_ptr, 2);
            if (r == 0)
            {
                sint64_t rc = 0;
                file* f = filesys::fdtable_get(process::cur_fds(), dirfd, &rc);
                if (!f)
                {
                    set(regs, rc);
                    return;
                }
                struct stat st;
                rc = f->vn->ops->getattr(f->vn, &st);
                if (rc == 0 && !uaccess::copy_to_user(stat_ptr, &st, sizeof(st)))
                    rc = -EFAULT;
                set(regs, rc);
                return;
            }
        }

        vnode* base = resolve_dirfd(dirfd, regs);
        if (!base)
            return;

        char* path = fetch_path(path_ptr, regs);
        if (!path)
            return;

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, base, &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        struct stat st;
        rc = v->ops->getattr(v, &st);
        vfs::unref(v);
        if (rc == 0 && !uaccess::copy_to_user(stat_ptr, &st, sizeof(st)))
            rc = -EFAULT;
        set(regs, rc);
    }

    // -----------------------------------------------------------------------
    // access / chmod / chown / umask / utimensat
    // -----------------------------------------------------------------------

    void do_access(vnode* base, const char* path, sint32_t mode, syscall_regs* regs)
    {
        if (mode & ~(R_OK | W_OK | X_OK | F_OK))
        {
            set(regs, ERR(EINVAL));
            return;
        }

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, base, &v, false);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        uint32_t perms = v->mode & 0777;
        bool ok = true;
        if (mode & R_OK)
            ok = ok && (perms & 0400) != 0;
        if (mode & W_OK)
            ok = ok && (perms & 0200) != 0;
        if (mode & X_OK)
        {
            // exec checks only S_IFREG (stage-3 rule); X_OK on a directory
            // is the search permission, which we always grant.
            if (v->type == vtype::REG)
                ok = ok && (perms & 0100) != 0;
        }
        vfs::unref(v);
        set(regs, ok ? 0 : ERR(EACCES));
    }

    void sys_access(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        do_access(process::cur_cwd(), path, (sint32_t)regs->rsi, regs);
        kfree(path);
    }

    void sys_faccessat(syscall_regs* regs, iret_frame*)
    {
        vnode* base = resolve_dirfd((sint32_t)regs->rdi, regs);
        if (!base)
            return;
        char* path = fetch_path(regs->rsi, regs);
        if (!path)
            return;
        do_access(base, path, (sint32_t)regs->rdx, regs);
        kfree(path);
    }

    void sys_chmod(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, process::cur_cwd(), &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        if (!v->ops->setattr)
        {
            vfs::unref(v);
            set(regs, ERR(EPERM));
            return;
        }
        rc = v->ops->setattr(v, (uint32_t)regs->rsi);
        vfs::unref(v);
        set(regs, rc);
    }

    void sys_fchmod(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        if (!f->vn->ops->setattr)
        {
            set(regs, ERR(EPERM));
            return;
        }
        set(regs, f->vn->ops->setattr(f->vn, (uint32_t)regs->rsi));
    }

    void sys_chown(syscall_regs* regs, iret_frame*)
    {
        // uid/gid are root-only fiction here: succeed like Linux does for
        // a process that owns the file and asks for the current ids.
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, process::cur_cwd(), &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        vfs::unref(v);
        set(regs, 0);
    }

    void sys_fchown(syscall_regs* regs, iret_frame*)
    {
        (void)regs;
        sint64_t rc = 0;
        if (!filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc))
        {
            set(regs, rc);
            return;
        }
        set(regs, 0);
    }

    void sys_umask(syscall_regs* regs, iret_frame*)
    {
        uint32_t old = process::cur_umask();
        process::set_umask((uint32_t)regs->rdi);
        set(regs, (sint64_t)old);
    }

    void sys_utimensat(syscall_regs* regs, iret_frame*)
    {
        sint32_t dirfd = (sint32_t)regs->rdi;
        uint64_t path_ptr = regs->rsi;
        uint64_t times_ptr = regs->rdx;

        // times == NULL: set to now (the FAT driver derives times from the
        // vnode on writeback; without setattr support for times we accept
        // and ignore, like a read-only FS would).
        if (times_ptr && !uaccess::readable(times_ptr, 4 * sizeof(sint64_t)))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        vnode* base = resolve_dirfd(dirfd, regs);
        if (!base)
            return;

        char* path = fetch_path(path_ptr, regs);
        if (!path)
            return;

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, base, &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        if (times_ptr)
        {
            sint64_t ts[4];
            if (!uaccess::copy_from_user(ts, times_ptr, sizeof(ts)))
            {
                vfs::unref(v);
                set(regs, ERR(EFAULT));
                return;
            }
            // ts = { atime_sec, atime_nsec, mtime_sec, mtime_nsec }.
            // UTIME_NOW (-1) / UTIME_OMIT (-2) are honoured coarsely.
            if (ts[2] >= 0)
            {
                v->mtime = (uint64_t)ts[2];
                vfs::touch(v, false);
            }
            if (ts[0] >= 0)
                v->atime = (uint64_t)ts[0];
        }
        else
        {
            vfs::touch(v, true);
        }

        vfs::unref(v);
        set(regs, 0);
    }

    // -----------------------------------------------------------------------
    // Directories: getdents64, mkdir(at), rmdir, unlink(at), rename(at/2),
    // getcwd, chdir, fchdir, readlink(at)
    // -----------------------------------------------------------------------

    void sys_getdents64(syscall_regs* regs, iret_frame*)
    {
        sint32_t fd = (sint32_t)regs->rdi;
        uint64_t user_buf = regs->rsi;
        uint32_t count = (uint32_t)regs->rdx;

        if (count == 0)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (!uaccess::writable(user_buf, count))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), fd, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        if (f->vn->type != vtype::DIR)
        {
            set(regs, ERR(ENOTDIR));
            return;
        }

        // Fill the bounce buffer with linux_dirent64 records, then one copy
        // to user space. The file offset doubles as the readdir cookie.
        uint64_t cookie = f->offset;
        uint64_t used = 0;

        while (used < BOUNCE_SIZE)
        {
            dirent_out d;
            bool eof = false;
            rc = f->vn->ops->readdir(f->vn, &cookie, &d, &eof);
            if (rc != 0)
                break;
            if (eof)
                break;

            uint32_t nlen = strlen(d.name);
            uint32_t reclen = (uint32_t)(offsetof_dirent() + nlen + 1 + 7) & ~7u;
            if (used + reclen > BOUNCE_SIZE || used + reclen > count)
                break;              // does not fit: leave for the next call

            linux_dirent64* de = (linux_dirent64*)(bounce + used);
            de->d_ino = d.ino;
            de->d_off = (sint64_t)cookie;   // position AFTER this entry
            de->d_reclen = (uint16_t)reclen;
            de->d_type = d.type;
            memory::memcpy(bounce + used + offsetof_dirent(),
                           (const uint8_t*)d.name, nlen + 1);
            // Pad to the record boundary.
            for (uint32_t i = nlen + 1; i < reclen - offsetof_dirent(); i++)
                bounce[used + offsetof_dirent() + i] = 0;

            used += reclen;
        }

        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        f->offset = cookie;
        if (used == 0)
        {
            set(regs, 0);           // end of directory
            return;
        }
        if (!uaccess::copy_to_user(user_buf, bounce, used))
        {
            set(regs, ERR(EFAULT));
            return;
        }
        set(regs, (sint64_t)used);
    }

    void sys_mkdirat(syscall_regs* regs, iret_frame*)
    {
        vnode* base = resolve_dirfd((sint32_t)regs->rdi, regs);
        if (!base)
            return;
        char* path = fetch_path(regs->rsi, regs);
        if (!path)
            return;

        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, base, &parent, name);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        uint32_t mode = ((uint32_t)regs->rdx) & ~process::cur_umask();
        rc = parent->ops->mkdir(parent, name, mode);
        vfs::unref(parent);
        set(regs, rc);
    }

    void sys_mkdir(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;

        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, process::cur_cwd(), &parent, name);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        uint32_t mode = ((uint32_t)regs->rsi) & ~process::cur_umask();
        rc = parent->ops->mkdir(parent, name, mode);
        vfs::unref(parent);
        set(regs, rc);
    }

    void sys_unlinkat(syscall_regs* regs, iret_frame*)
    {
        sint32_t dirfd = (sint32_t)regs->rdi;
        sint32_t flags = (sint32_t)regs->rdx;

        vnode* base = resolve_dirfd(dirfd, regs);
        if (!base)
            return;
        char* path = fetch_path(regs->rsi, regs);
        if (!path)
            return;

        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, base, &parent, name);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }

        if (flags & AT_REMOVEDIR)
            rc = parent->ops->rmdir(parent, name);
        else
            rc = parent->ops->unlink(parent, name);
        vfs::unref(parent);
        set(regs, rc);
    }

    void sys_rmdir(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, process::cur_cwd(), &parent, name);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        rc = parent->ops->rmdir(parent, name);
        vfs::unref(parent);
        set(regs, rc);
    }

    void sys_unlink(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, process::cur_cwd(), &parent, name);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        rc = parent->ops->unlink(parent, name);
        vfs::unref(parent);
        set(regs, rc);
    }

    void sys_renameat2(syscall_regs* regs, iret_frame*)
    {
        vnode* od = resolve_dirfd((sint32_t)regs->rdi, regs);
        if (!od)
            return;
        vnode* nd = resolve_dirfd((sint32_t)regs->rdx, regs);
        if (!nd)
            return;
        do_rename(od, regs->rsi, nd, regs->r10, (uint32_t)regs->r8, regs);
    }

    // Shared body of rename/renameat/renameat2: all three differ only in
    // where the four path/fd arguments sit.
    void do_rename(vnode* old_dir, uint64_t oldp, vnode* new_dir, uint64_t newp,
                   uint32_t flags, syscall_regs* regs)
    {
        if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        {
            set(regs, ERR(EINVAL));
            return;
        }

        char* op = fetch_path(oldp, regs);
        if (!op)
            return;
        char* np = fetch_path(newp, regs);
        if (!np)
        {
            kfree(op);
            return;
        }

        vnode* od = nullptr;
        vnode* nd = nullptr;
        char oname[NAME_MAX + 1];
        char nname[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(op, old_dir, &od, oname);
        if (rc == 0)
            rc = vfs::lookup_parent(np, new_dir, &nd, nname);
        kfree(op);
        kfree(np);

        if (rc == 0)
            rc = od->ops->rename(od, oname, nd, nname, flags);

        if (od)
            vfs::unref(od);
        if (nd)
            vfs::unref(nd);
        set(regs, rc);
    }

    void sys_rename(syscall_regs* regs, iret_frame*)
    {
        do_rename(process::cur_cwd(), regs->rdi, process::cur_cwd(),
                  regs->rsi, 0, regs);
    }

    void sys_renameat(syscall_regs* regs, iret_frame*)
    {
        vnode* od = resolve_dirfd((sint32_t)regs->rdi, regs);
        if (!od)
            return;
        vnode* nd = resolve_dirfd((sint32_t)regs->rdx, regs);
        if (!nd)
            return;
        do_rename(od, regs->rsi, nd, regs->r10, 0, regs);
    }

    void sys_getcwd(syscall_regs* regs, iret_frame*)
    {
        uint64_t buf = regs->rdi;
        uint64_t size = regs->rsi;

        if (size == 0)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if (!uaccess::writable(buf, size))
        {
            set(regs, ERR(EFAULT));
            return;
        }

        char* p = (char*)kmalloc(PATH_MAX);
        if (!p)
        {
            set(regs, ERR(ENOMEM));
            return;
        }

        sint64_t rc = vfs::get_path(process::cur_cwd(), p, PATH_MAX, nullptr);
        if (rc != 0)
        {
            kfree(p);
            set(regs, rc);
            return;
        }

        uint32_t len = strlen(p) + 1;
        if (len > size)
        {
            kfree(p);
            set(regs, ERR(ERANGE));
            return;
        }

        if (!uaccess::copy_to_user(buf, p, len))
        {
            kfree(p);
            set(regs, ERR(EFAULT));
            return;
        }
        kfree(p);
        set(regs, (sint64_t)len);
    }

    void sys_chdir(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, process::cur_cwd(), &v, true);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        process::set_cwd(v);        // takes the reference
        set(regs, 0);
    }

    void sys_fchdir(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        if (f->vn->type != vtype::DIR)
        {
            set(regs, ERR(ENOTDIR));
            return;
        }
        vfs::ref(f->vn);
        process::set_cwd(f->vn);
        set(regs, 0);
    }

    void sys_readlinkat(syscall_regs* regs, iret_frame*)
    {
        (void)regs;
        set(regs, ERR(EINVAL));     // no symlinks (stage-3 decision)
    }

    void sys_readlink(syscall_regs* regs, iret_frame*)
    {
        (void)regs;
        set(regs, ERR(EINVAL));
    }

    // -----------------------------------------------------------------------
    // truncate / ftruncate / fsync / fdatasync / sync
    // -----------------------------------------------------------------------

    void sys_truncate(syscall_regs* regs, iret_frame*)
    {
        char* path = fetch_path(regs->rdi, regs);
        if (!path)
            return;
        sint64_t len = (sint64_t)regs->rsi;
        if (len < 0)
        {
            kfree(path);
            set(regs, ERR(EINVAL));
            return;
        }

        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, process::cur_cwd(), &v, false);
        kfree(path);
        if (rc != 0)
        {
            set(regs, rc);
            return;
        }
        if (v->type == vtype::DIR)
        {
            vfs::unref(v);
            set(regs, ERR(EISDIR));
            return;
        }
        rc = v->ops->truncate(v, (uint64_t)len);
        vfs::unref(v);
        set(regs, rc);
    }

    void sys_ftruncate(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        sint64_t len = (sint64_t)regs->rsi;
        if (len < 0)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        if ((f->flags & O_ACCMODE) == O_RDONLY)
        {
            set(regs, ERR(EINVAL));       // Linux: EBADF/EINVAL for O_RDONLY
            return;
        }
        if (f->vn->type == vtype::DIR)
        {
            set(regs, ERR(EINVAL));
            return;
        }
        rc = f->vn->ops->truncate(f->vn, (uint64_t)len);
        if (rc == 0 && f->offset > (uint64_t)len)
            f->offset = (uint64_t)len;
        set(regs, rc);
    }

    void sys_fsync(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        rc = f->vn->ops->fsync ? f->vn->ops->fsync(f->vn) : 0;
        set(regs, rc);
    }

    void sys_fdatasync(syscall_regs* regs, iret_frame*)
    {
        sys_fsync(regs, nullptr);       // same on FAT: data + entry
    }

    void sys_sync(syscall_regs* regs, iret_frame*)
    {
        set(regs, vfs::sync_all());
    }

    // -----------------------------------------------------------------------
    // ioctl
    // -----------------------------------------------------------------------

    void sys_ioctl(syscall_regs* regs, iret_frame*)
    {
        sint64_t rc = 0;
        file* f = filesys::fdtable_get(process::cur_fds(), (sint32_t)regs->rdi, &rc);
        if (!f)
        {
            set(regs, rc);
            return;
        }
        if (!f->vn->ops->ioctl)
        {
            set(regs, ERR(ENOTTY));
            return;
        }

        uint64_t request = regs->rsi;
        uint64_t arg = regs->rdx;

        // Requests that pass a pointer need a kernel-side bounce: the FS
        // layer must never see a user pointer.
        if (request == TCGETS)
        {
            if (!uaccess::writable(arg, sizeof(struct termios)))
            {
                set(regs, ERR(EFAULT));
                return;
            }
            struct termios t;
            rc = f->vn->ops->ioctl(f->vn, request, (uint64_t)(uintptr_t)&t);
            if (rc == 0 && !uaccess::copy_to_user(arg, &t, sizeof(t)))
                rc = -EFAULT;
            set(regs, rc);
            return;
        }
        if (request == TIOCGWINSZ)
        {
            if (!uaccess::writable(arg, sizeof(struct winsize)))
            {
                set(regs, ERR(EFAULT));
                return;
            }
            struct winsize w;
            rc = f->vn->ops->ioctl(f->vn, request, (uint64_t)(uintptr_t)&w);
            if (rc == 0 && !uaccess::copy_to_user(arg, &w, sizeof(w)))
                rc = -EFAULT;
            set(regs, rc);
            return;
        }

        set(regs, f->vn->ops->ioctl(f->vn, request, arg));
    }

}

namespace sys_fs
{
    void register_handlers()
    {
        syscall::set_handler(SYS_READ,        sys_read);
        syscall::set_handler(SYS_WRITE,       sys_write);
        syscall::set_handler(SYS_OPEN,        sys_open);
        syscall::set_handler(SYS_CLOSE,       sys_close);
        syscall::set_handler(SYS_STAT,        sys_stat);
        syscall::set_handler(SYS_FSTAT,       sys_fstat);
        syscall::set_handler(SYS_LSTAT,       sys_lstat);
        syscall::set_handler(SYS_LSEEK,       sys_lseek);
        syscall::set_handler(SYS_IOCTL,       sys_ioctl);
        syscall::set_handler(SYS_PREAD64,     sys_pread);
        syscall::set_handler(SYS_PWRITE64,    sys_pwrite);
        syscall::set_handler(SYS_READV,       sys_readv);
        syscall::set_handler(SYS_WRITEV,      sys_writev);
        syscall::set_handler(SYS_ACCESS,      sys_access);
        syscall::set_handler(SYS_DUP,         sys_dup);
        syscall::set_handler(SYS_DUP2,        sys_dup2);
        syscall::set_handler(SYS_FCNTL,       sys_fcntl);
        syscall::set_handler(SYS_FSYNC,       sys_fsync);
        syscall::set_handler(SYS_FDATASYNC,   sys_fdatasync);
        syscall::set_handler(SYS_TRUNCATE,    sys_truncate);
        syscall::set_handler(SYS_FTRUNCATE,   sys_ftruncate);
        syscall::set_handler(SYS_GETCWD,      sys_getcwd);
        syscall::set_handler(SYS_CHDIR,       sys_chdir);
        syscall::set_handler(SYS_FCHDIR,      sys_fchdir);
        syscall::set_handler(SYS_RENAME,      sys_rename);
        syscall::set_handler(SYS_MKDIR,       sys_mkdir);
        syscall::set_handler(SYS_RMDIR,       sys_rmdir);
        syscall::set_handler(SYS_CREAT,       sys_creat);
        syscall::set_handler(SYS_UNLINK,      sys_unlink);
        syscall::set_handler(SYS_READLINK,    sys_readlink);
        syscall::set_handler(SYS_CHMOD,       sys_chmod);
        syscall::set_handler(SYS_FCHMOD,      sys_fchmod);
        syscall::set_handler(SYS_CHOWN,       sys_chown);
        syscall::set_handler(SYS_FCHOWN,      sys_fchown);
        syscall::set_handler(SYS_UMASK,       sys_umask);
        syscall::set_handler(SYS_SYNC,        sys_sync);
        syscall::set_handler(SYS_GETDENTS64,  sys_getdents64);
        syscall::set_handler(SYS_OPENAT,      sys_openat);
        syscall::set_handler(SYS_MKDIRAT,     sys_mkdirat);
        syscall::set_handler(SYS_NEWFSTATAT,  sys_newfstatat);
        syscall::set_handler(SYS_UNLINKAT,    sys_unlinkat);
        syscall::set_handler(SYS_RENAMEAT,    sys_renameat);
        syscall::set_handler(SYS_READLINKAT,  sys_readlinkat);
        syscall::set_handler(SYS_FACCESSAT,   sys_faccessat);
        syscall::set_handler(SYS_UTIMENSAT,   sys_utimensat);
        syscall::set_handler(SYS_DUP3,        sys_dup3);
        syscall::set_handler(SYS_RENAMEAT2,   sys_renameat2);
    }
}
