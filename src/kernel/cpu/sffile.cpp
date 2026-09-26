// Files through the SurfaceOS SDK. See sffile.h.

#include "../../include/cpu/sffile.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/sys_fs.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/mm/heap.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/fcntl.h"
#include "../../sdk/include/sfos.h"

namespace
{
    const uint64_t PATH_SIZE = 1024;    // longest path a call takes, NUL included

    // The SfStatus for a kernel -errno.
    SfStatus status(sint64_t rc)
    {
        switch (rc)
        {
            case 0:         return SF_SUCCESS;
            case -ENOENT:
            case -ENOTDIR:  return SF_NOT_FOUND;
            case -EEXIST:   return SF_ALREADY_EXISTS;
            case -EXDEV:
            case -EACCES:
            case -EPERM:
            case -EROFS:    return SF_ACCESS_DENIED;
            case -ENOMEM:
            case -EMFILE:
            case -ENFILE:
            case -ENOSPC:   return SF_OUT_OF_RESOURCES;
            case -EBADF:    return SF_BAD_HANDLE;
            case -EFAULT:
            case -EINVAL:
            case -EISDIR:
            case -ENAMETOOLONG: return SF_INVALID_PARAMETER;
            default:        return SF_DEVICE_ERROR;
        }
    }

    // The file behind handle h, or nullptr.
    file* from_handle(uint64_t h)
    {
        if (h >= HANDLE_TABLE_SIZE)
            return nullptr;
        sint64_t rc;
        return filesys::fd_get(process::cur_handles(), (sint32_t)h, &rc);
    }

    // Hand the new handle h to the program at user_out; on failure the
    // handle is closed again.
    SfStatus give_handle(sint64_t h, uint64_t user_out)
    {
        uint64_t v = (uint64_t)h;
        if (!uaccess::copy_to_user(user_out, &v, sizeof(v)))
        {
            filesys::fd_close(process::cur_handles(), (sint32_t)h);
            return SF_INVALID_PARAMETER;
        }
        return SF_SUCCESS;
    }

    // Copy a path in from the program: a kmalloc'ed string, or nullptr.
    char* fetch_path(uint64_t user, SfStatus* st)
    {
        char* path = (char*)kmalloc(PATH_SIZE);
        if (!path)
        {
            *st = SF_OUT_OF_RESOURCES;
            return nullptr;
        }
        if (uaccess::strncpy_from_user(path, user, PATH_SIZE) < 0)
        {
            kfree(path);
            *st = SF_INVALID_PARAMETER;
            return nullptr;
        }
        return path;
    }

    // The open flags for SF_FILE_* `mode`, or -1 for a bad mode.
    sint32_t open_flags(uint64_t mode)
    {
        const uint64_t known = SF_FILE_READ | SF_FILE_WRITE | SF_FILE_CREATE |
                               SF_FILE_CREATE_NEW | SF_FILE_TRUNCATE;
        if (mode & ~known)
            return -1;

        sint32_t flags;
        if ((mode & SF_FILE_READ) && (mode & SF_FILE_WRITE))
            flags = O_RDWR;
        else if (mode & SF_FILE_WRITE)
            flags = O_WRONLY;
        else if (mode & SF_FILE_READ)
            flags = O_RDONLY;
        else
            return -1;
        if (mode & SF_FILE_CREATE)
            flags |= O_CREAT;
        if (mode & SF_FILE_CREATE_NEW)
            flags |= O_CREAT | O_EXCL;
        if (mode & SF_FILE_TRUNCATE)
            flags |= O_TRUNC;
        return flags;
    }

    // Open `path` below `base` with SF_FILE_* `mode`; *out gets the handle.
    SfStatus open_below(vnode* base, const char* path, uint64_t mode, uint64_t user_out)
    {
        sint32_t flags = open_flags(mode);
        if (flags < 0)
            return SF_INVALID_PARAMETER;

        sint64_t h = sys_fs::open_at(base, path[0] ? path : ".", flags,
                                     0644, vfs::LOOKUP_BENEATH);
        if (h < 0)
            return status(h);
        return give_handle(h, user_out);
    }

    // A root that is a file (argN:) opened as itself.
    SfStatus open_root_file(vnode* v, uint64_t mode, uint64_t user_out)
    {
        sint32_t flags = open_flags(mode);
        if (flags < 0)
            return SF_INVALID_PARAMETER;
        if (mode & SF_FILE_CREATE_NEW)
            return SF_ALREADY_EXISTS;
        sint64_t h = sys_fs::open_vnode(v, flags & ~O_CREAT);
        if (h < 0)
            return status(h);
        return give_handle(h, user_out);
    }

    // (Path, Mode, *Handle): "root:/path".
    void files_open(user_regs* regs, iret_frame*)
    {
        SfStatus st = SF_SUCCESS;
        char* path = fetch_path(regs->rdi, &st);
        if (!path)
        {
            regs->rax = st;
            return;
        }

        // Split off the root's name; the slashes after the colon belong to
        // the root.
        char* rest = path;
        while (*rest && *rest != ':' && *rest != '/')
            rest++;
        if (*rest != ':' || rest == path)
        {
            kfree(path);
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        *rest++ = '\0';
        while (*rest == '/')
            rest++;

        vnode* root = process::cur_root(path);
        if (!root)
            regs->rax = SF_NOT_FOUND;
        else if (root->type == vtype::DIR || *rest)
            regs->rax = open_below(root, rest, regs->rsi, regs->rdx);
        else
            regs->rax = open_root_file(root, regs->rsi, regs->rdx);
        kfree(path);
    }

    // (*Handle, Path, PathSize).
    void files_create_unique(user_regs* regs, iret_frame*)
    {
        vnode* tmp = process::cur_root("tmp");
        if (!tmp)
        {
            regs->rax = SF_NOT_FOUND;
            return;
        }

        // <number>.tmp, counting on from the last one handed out; a name
        // some other file has already is skipped.
        static uint64_t next = 1;
        for (uint32_t tries = 0; tries < 1000; tries++)
        {
            char name[32];
            char digits[20];
            uint32_t nd = 0, n = 0;
            for (uint64_t v = next++; v; v /= 10)
                digits[nd++] = (char)('0' + v % 10);
            while (nd)
                name[n++] = digits[--nd];
            for (const char* s = ".tmp"; *s; s++)
                name[n++] = *s;
            name[n] = '\0';

            sint64_t h = sys_fs::open_at(tmp, name, O_RDWR | O_CREAT | O_EXCL,
                                         0644, vfs::LOOKUP_BENEATH);
            if (h == -EEXIST)
                continue;
            if (h < 0)
            {
                regs->rax = status(h);
                return;
            }

            // The path back to the program: "tmp:/<name>".
            bool ok = true;
            if (regs->rsi)
            {
                char full[40] = "tmp:/";
                uint32_t len = 5;
                for (uint32_t i = 0; name[i]; i++)
                    full[len++] = name[i];
                full[len++] = '\0';
                ok = regs->rdx >= len && uaccess::copy_to_user(regs->rsi, full, len);
            }
            // On failure the name stays taken: the file exists, empty.
            if (!ok)
            {
                filesys::fd_close(process::cur_handles(), (sint32_t)h);
                regs->rax = SF_INVALID_PARAMETER;
                return;
            }
            regs->rax = give_handle(h, regs->rdi);
            return;
        }
        regs->rax = SF_OUT_OF_RESOURCES;
    }

    // (Dir, Path, Mode, *Handle): relative to a directory.
    void file_open(user_regs* regs, iret_frame*)
    {
        file* dir = from_handle(regs->rdi);
        if (!dir)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        SfStatus st = SF_SUCCESS;
        char* path = fetch_path(regs->rsi, &st);
        if (!path)
        {
            regs->rax = st;
            return;
        }
        regs->rax = open_below(dir->vn, path, regs->rdx, regs->r10);
        kfree(path);
    }

    void file_close(user_regs* regs, iret_frame*)
    {
        if (!from_handle(regs->rdi))
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        regs->rax = status(filesys::fd_close(process::cur_handles(), (sint32_t)regs->rdi));
    }

    // Read and Write: (Handle, Buffer, *Size) - *Size in and out.
    void file_transfer(user_regs* regs, bool write)
    {
        file* f = from_handle(regs->rdi);
        if (!f)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        uint64_t size = 0;
        if (!uaccess::copy_from_user(&size, regs->rdx, sizeof(size)))
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }
        sint64_t n = write ? sys_fs::write(f, regs->rsi, size)
                           : sys_fs::read(f, regs->rsi, size);
        if (n < 0)
        {
            regs->rax = status(n);
            return;
        }
        size = (uint64_t)n;
        regs->rax = uaccess::copy_to_user(regs->rdx, &size, sizeof(size))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    void file_read(user_regs* regs, iret_frame*)  { file_transfer(regs, false); }
    void file_write(user_regs* regs, iret_frame*) { file_transfer(regs, true); }

    void file_get_position(user_regs* regs, iret_frame*)
    {
        file* f = from_handle(regs->rdi);
        if (!f)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        regs->rax = uaccess::copy_to_user(regs->rsi, &f->offset, sizeof(f->offset))
                  ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }

    void file_set_position(user_regs* regs, iret_frame*)
    {
        file* f = from_handle(regs->rdi);
        if (!f)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        if (f->vn->type != vtype::REG)
        {
            regs->rax = SF_UNSUPPORTED;     // a folder or a device
            return;
        }
        f->offset = regs->rsi;
        regs->rax = SF_SUCCESS;
    }

    // Look up (and, when missing, create) directory `name` in `parent`.
    sint64_t subdir(vnode* parent, const char* name, vnode** out)
    {
        sint64_t rc = parent->ops->lookup(parent, name, out);
        if (rc == -ENOENT && parent->ops->mkdir)
        {
            rc = parent->ops->mkdir(parent, name, 0755);
            if (rc == 0)
            {
                uart::printf("sffile: created /files/%s\n", name);
                rc = parent->ops->lookup(parent, name, out);
            }
        }
        if (rc == 0 && (*out)->type != vtype::DIR)
        {
            vfs::unref(*out);
            *out = nullptr;
            rc = -ENOTDIR;
        }
        return rc;
    }
}

namespace sffile
{
    void open_roots(const char* name, vnode** data, vnode** tmp)
    {
        *data = nullptr;
        *tmp  = nullptr;

        vnode* files = nullptr;
        if (vfs::lookup("/files", nullptr, &files, true) == 0)
        {
            sint64_t rc = subdir(files, name, data);
            if (rc != 0)
                uart::printf("sffile: no data:/ for %s (%d)\n", name, (int)rc);
            vfs::unref(files);
        }
        if (vfs::lookup("/tmp", nullptr, tmp, true) != 0)
            *tmp = nullptr;
    }

    void init()
    {
        sfcall::set_handler(SFCALL_FILES_OPEN, files_open);
        sfcall::set_handler(SFCALL_FILES_CREATE_UNIQUE, files_create_unique);
        sfcall::set_handler(SFCALL_FILE_OPEN, file_open);
        sfcall::set_handler(SFCALL_FILE_CLOSE, file_close);
        sfcall::set_handler(SFCALL_FILE_READ, file_read);
        sfcall::set_handler(SFCALL_FILE_WRITE, file_write);
        sfcall::set_handler(SFCALL_FILE_GET_POSITION, file_get_position);
        sfcall::set_handler(SFCALL_FILE_SET_POSITION, file_set_position);
    }
}
