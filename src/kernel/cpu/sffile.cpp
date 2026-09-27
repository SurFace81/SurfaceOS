// Files through the SurfaceOS SDK. See sffile.h.

#include "../../include/cpu/sffile.h"
#include "../../include/cpu/sfcall.h"
#include "../../include/fs/fileio.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/file.h"
#include "../../include/mm/heap.h"
#include "../../sdk/include/abi/stat.h"
#include "../../include/mm/memory.h"
#include "../../include/stdlib/string.h"
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
            case -ENOTEMPTY:
            case -EBUSY:    return SF_IN_USE;
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
    // A path that leads back to `forbid` is SF_ACCESS_DENIED.
    SfStatus open_below(vnode* base, const char* path, uint64_t mode, uint64_t user_out,
                        vnode* forbid = nullptr)
    {
        sint32_t flags = open_flags(mode);
        if (flags < 0)
            return SF_INVALID_PARAMETER;

        sint64_t h = fileio::open_at(base, path[0] ? path : ".", flags,
                                     0644, vfs::LOOKUP_BENEATH);
        if (h < 0)
            return status(h);
        sint64_t rc;
        file* f = filesys::fd_get(process::cur_handles(), (sint32_t)h, &rc);
        if (forbid && f && f->vn == forbid)
        {
            filesys::fd_close(process::cur_handles(), (sint32_t)h);
            return SF_ACCESS_DENIED;
        }
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
        sint64_t h = fileio::open_vnode(v, flags & ~O_CREAT);
        if (h < 0)
            return status(h);
        return give_handle(h, user_out);
    }

    // "root:/path" in `path` (changed in place): the caller's root, and
    // what comes after it in *rest. nullptr (and *st) when there is none.
    vnode* split_root(char* path, char** rest, SfStatus* st)
    {
        // The slashes after the colon belong to the root.
        char* r = path;
        while (*r && *r != ':' && *r != '/')
            r++;
        if (*r != ':' || r == path)
        {
            *st = SF_INVALID_PARAMETER;
            return nullptr;
        }
        *r++ = '\0';
        while (*r == '/')
            r++;
        *rest = r;
        vnode* root = process::cur_root(path);
        if (!root)
            *st = SF_NOT_FOUND;
        return root;
    }

    // The folder that `root:/rest` names the last part of, referenced, and
    // that part in `name` (NAME_MAX + 1 bytes). The root itself has none.
    vnode* parent_of(vnode* root, const char* rest, char* name, SfStatus* st)
    {
        if (!*rest || root->type != vtype::DIR)
        {
            *st = SF_ACCESS_DENIED;         // the root itself
            return nullptr;
        }
        vnode* dir = nullptr;
        sint64_t rc = vfs::lookup_parent(rest, root, &dir, name, vfs::LOOKUP_BENEATH);
        if (rc == 0 && (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0))
        {
            vfs::unref(dir);
            rc = -EACCES;
        }
        if (rc != 0)
        {
            *st = status(rc);
            return nullptr;
        }
        return dir;
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

        char* rest = nullptr;
        vnode* root = split_root(path, &rest, &st);
        if (!root)
            regs->rax = st;
        else if (root->type == vtype::DIR || *rest)
        {
            // tmp:/ itself stays closed: its contents are not to be listed.
            vnode* forbid = strcmp(path, "tmp") == 0 ? root : nullptr;
            regs->rax = open_below(root, rest, regs->rsi, regs->rdx, forbid);
        }
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

            sint64_t h = fileio::open_at(tmp, name, O_RDWR | O_CREAT | O_EXCL,
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

    // (Path): make a folder.
    void files_create_directory(user_regs* regs, iret_frame*)
    {
        SfStatus st = SF_SUCCESS;
        char* path = fetch_path(regs->rdi, &st);
        char* rest = nullptr;
        char name[NAME_MAX + 1];
        vnode* root = path ? split_root(path, &rest, &st) : nullptr;
        vnode* dir = root ? parent_of(root, rest, name, &st) : nullptr;
        if (dir)
        {
            st = dir->ops->mkdir ? status(dir->ops->mkdir(dir, name, 0755)) : SF_ACCESS_DENIED;
            vfs::unref(dir);
        }
        if (path)
            kfree(path);
        regs->rax = st;
    }

    // (Path): remove a file, or an empty folder.
    void files_delete(user_regs* regs, iret_frame*)
    {
        SfStatus st = SF_SUCCESS;
        char* path = fetch_path(regs->rdi, &st);
        char* rest = nullptr;
        char name[NAME_MAX + 1];
        vnode* root = path ? split_root(path, &rest, &st) : nullptr;
        vnode* dir = root ? parent_of(root, rest, name, &st) : nullptr;
        if (dir)
        {
            vnode* v = nullptr;
            sint64_t rc = dir->ops->lookup(dir, name, &v);
            if (rc == 0)
            {
                bool folder = v->type == vtype::DIR;
                vfs::unref(v);
                rc = folder ? (dir->ops->rmdir ? dir->ops->rmdir(dir, name) : -EPERM)
                            : (dir->ops->unlink ? dir->ops->unlink(dir, name) : -EPERM);
            }
            st = status(rc);
            vfs::unref(dir);
        }
        if (path)
            kfree(path);
        regs->rax = st;
    }

    // (OldPath, NewPath): on one volume, never over something that exists.
    void files_rename(user_regs* regs, iret_frame*)
    {
        SfStatus st = SF_SUCCESS;
        char* from = fetch_path(regs->rdi, &st);
        char* to = from ? fetch_path(regs->rsi, &st) : nullptr;
        char* from_rest = nullptr;
        char* to_rest = nullptr;
        char from_name[NAME_MAX + 1], to_name[NAME_MAX + 1];
        vnode* from_root = to ? split_root(from, &from_rest, &st) : nullptr;
        vnode* to_root = from_root ? split_root(to, &to_rest, &st) : nullptr;
        vnode* od = to_root ? parent_of(from_root, from_rest, from_name, &st) : nullptr;
        vnode* nd = od ? parent_of(to_root, to_rest, to_name, &st) : nullptr;
        if (nd)
        {
            if (od->mnt != nd->mnt)
                st = SF_ACCESS_DENIED;          // another volume: copy it
            else
                st = od->ops->rename ? status(od->ops->rename(od, from_name, nd, to_name,
                                                              RENAME_NOREPLACE))
                                     : SF_ACCESS_DENIED;
        }
        if (nd)   vfs::unref(nd);
        if (od)   vfs::unref(od);
        if (to)   kfree(to);
        if (from) kfree(from);
        regs->rax = st;
    }

    // Epoch seconds as an SfDateTime (UTC, what the clock keeps).
    void to_date_time(uint64_t epoch, SfDateTime* out)
    {
        uint32_t secs = (uint32_t)(epoch % 86400);
        sint64_t z = (sint64_t)(epoch / 86400) + 719468;
        sint64_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t m = mp + (mp < 10 ? 3u : (uint32_t)-9);
        out->Year   = (uint16_t)((sint64_t)yoe + era * 400 + (m <= 2));
        out->Month  = (uint8_t)m;
        out->Day    = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
        out->Hour   = (uint8_t)(secs / 3600);
        out->Minute = (uint8_t)(secs / 60 % 60);
        out->Second = (uint8_t)(secs % 60);
        out->Reserved = 0;
    }

    // What v is, into e (its name left as it is).
    void describe(vnode* v, SfDirEntry* e)
    {
        struct stat st;
        memory::memset((uint8_t*)&st, 0, sizeof(st));
        if (v->ops->getattr)
            v->ops->getattr(v, &st);
        bool folder = v->type == vtype::DIR;
        e->Size  = folder ? 0 : (uint64_t)st.st_size;
        e->Flags = folder ? SF_DIR_ENTRY_FOLDER : 0;
        to_date_time((uint64_t)st.st_mtim.tv_sec, &e->Modified);
        e->Reserved = 0;
    }

    // (Handle, *Entry): the next entry of a folder, its position the
    // file's.
    void file_read_dir(user_regs* regs, iret_frame*)
    {
        file* f = from_handle(regs->rdi);
        if (!f)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        vnode* dir = f->vn;
        if (dir->type != vtype::DIR || !dir->ops->readdir)
        {
            regs->rax = SF_UNSUPPORTED;
            return;
        }
        // tmp:/ is not to be listed (files.h).
        if (dir == process::cur_root("tmp"))
        {
            regs->rax = SF_ACCESS_DENIED;
            return;
        }

        for (;;)
        {
            dirent_out d;
            bool eof = false;
            uint64_t cookie = f->offset;
            sint64_t rc = dir->ops->readdir(dir, &cookie, &d, &eof);
            if (rc != 0)
            {
                regs->rax = status(rc);
                return;
            }
            if (eof)
            {
                regs->rax = SF_END_OF_FILE;
                return;
            }
            f->offset = cookie;
            if (strcmp(d.name, ".") == 0 || strcmp(d.name, "..") == 0)
                continue;

            SfDirEntry* e = (SfDirEntry*)kmalloc(sizeof(SfDirEntry));
            if (!e)
            {
                regs->rax = SF_OUT_OF_RESOURCES;
                return;
            }
            memory::memset((uint8_t*)e, 0, sizeof(*e));
            strncpy(e->Name, d.name, sizeof(e->Name) - 1);
            vnode* child = nullptr;
            if (dir->ops->lookup(dir, d.name, &child) == 0)
            {
                describe(child, e);
                vfs::unref(child);
            }
            regs->rax = uaccess::copy_to_user(regs->rsi, e, sizeof(*e)) ? SF_SUCCESS
                                                                        : SF_INVALID_PARAMETER;
            kfree(e);
            return;
        }
    }

    // (Handle, *Info)
    void file_get_info(user_regs* regs, iret_frame*)
    {
        file* f = from_handle(regs->rdi);
        if (!f)
        {
            regs->rax = SF_BAD_HANDLE;
            return;
        }
        SfDirEntry* e = (SfDirEntry*)kmalloc(sizeof(SfDirEntry));
        if (!e)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }
        memory::memset((uint8_t*)e, 0, sizeof(*e));
        describe(f->vn, e);
        regs->rax = uaccess::copy_to_user(regs->rsi, e, sizeof(*e)) ? SF_SUCCESS
                                                                    : SF_INVALID_PARAMETER;
        kfree(e);
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
        sint64_t n = write ? fileio::write(f, regs->rsi, size)
                           : fileio::read(f, regs->rsi, size);
        if (n < 0)
        {
            // -EBADF here: the file is not open for this.
            regs->rax = n == -EBADF ? SF_ACCESS_DENIED : status(n);
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

    sint64_t lookup(const char* rooted, vnode** out)
    {
        *out = nullptr;
        SfStatus st = SF_SUCCESS;
        char* path = (char*)kmalloc(PATH_SIZE);
        if (!path)
            return -ENOMEM;
        strncpy(path, rooted, PATH_SIZE - 1);
        path[PATH_SIZE - 1] = '\0';
        char* rest = nullptr;
        vnode* root = split_root(path, &rest, &st);
        sint64_t rc = -ENOENT;
        if (root && !*rest)
        {
            vfs::ref(root);
            *out = root;
            rc = 0;
        }
        else if (root)
            rc = vfs::lookup(rest, root, out, false, vfs::LOOKUP_BENEATH);
        kfree(path);
        return rc;
    }

    void init()
    {
        sfcall::set_handler(SFCALL_FILES_OPEN, files_open);
        sfcall::set_handler(SFCALL_FILES_CREATE_UNIQUE, files_create_unique);
        sfcall::set_handler(SFCALL_FILE_OPEN, file_open);
        sfcall::set_handler(SFCALL_FILE_READ, file_read);
        sfcall::set_handler(SFCALL_FILE_WRITE, file_write);
        sfcall::set_handler(SFCALL_FILE_GET_POSITION, file_get_position);
        sfcall::set_handler(SFCALL_FILE_SET_POSITION, file_set_position);
        sfcall::set_handler(SFCALL_FILE_READ_DIR, file_read_dir);
        sfcall::set_handler(SFCALL_FILE_GET_INFO, file_get_info);
        sfcall::set_handler(SFCALL_FILES_CREATE_DIRECTORY, files_create_directory);
        sfcall::set_handler(SFCALL_FILES_DELETE, files_delete);
        sfcall::set_handler(SFCALL_FILES_RENAME, files_rename);
    }
}
