// devfs: /dev/null, /dev/zero, /dev/tty, /dev/console (stage 3.5).
// See devfs.h. Everything is in-memory; the vnodes are created once at
// mount and live until umount.

#include "../../include/fs/devfs.h"
#include "../../include/drivers/tty.h"
#include "../../include/mm/memory.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/stat.h"
#include "../../sdk/include/abi/dirent.h"
#include "../../sdk/include/abi/termios.h"

namespace
{
    struct dev_entry
    {
        const char* name;
        devfs::dev_id id;
        uint32_t mode;          // permissions
    };

    const dev_entry devices[] =
    {
        { "null",    devfs::DEV_NULL,    0666 },
        { "zero",    devfs::DEV_ZERO,    0666 },
        { "tty",     devfs::DEV_TTY,     0620 },
        { "console", devfs::DEV_CONSOLE, 0600 },
    };
    const uint32_t NDEV = sizeof(devices) / sizeof(devices[0]);

    vnode_ops dev_ops;          // forward-filled below

    // The four vnodes and the root, created once per mount.
    vnode* root_vn = nullptr;
    vnode* dev_vn[NDEV];

    devfs::dev_id id_of(vnode* v)
    {
        for (uint32_t i = 0; i < NDEV; i++)
            if (dev_vn[i] == v)
                return devices[i].id;
        return (devfs::dev_id)0;
    }

    // -----------------------------------------------------------------------
    // ops
    // -----------------------------------------------------------------------

    sint64_t dev_lookup(vnode* dir, const char* name, vnode** out)
    {
        (void)dir;
        for (uint32_t i = 0; i < NDEV; i++)
        {
            if (strcmp(devices[i].name, name) == 0)
            {
                vfs::ref(dev_vn[i]);
                *out = dev_vn[i];
                return 0;
            }
        }
        return -ENOENT;
    }

    sint64_t dev_getparent(vnode* v, vnode** out_parent, char* name_out)
    {
        if (v == root_vn)
        {
            vfs::ref(v);
            *out_parent = v;
            name_out[0] = '\0';
            return 0;
        }
        for (uint32_t i = 0; i < NDEV; i++)
            if (dev_vn[i] == v)
            {
                vfs::ref(root_vn);
                *out_parent = root_vn;
                strncpy(name_out, devices[i].name, NAME_MAX);
                name_out[NAME_MAX] = '\0';
                return 0;
            }
        return -ENOENT;
    }

    sint64_t dev_read(vnode* v, uint64_t off, void* buf, uint64_t len,
                      uint64_t* done)
    {
        (void)off;
        *done = 0;

        switch (id_of(v))
        {
            case devfs::DEV_NULL:
                return 0;                       // EOF

            case devfs::DEV_ZERO:
                memory::memset((uint8_t*)buf, 0, len);
                *done = len;
                return 0;

            case devfs::DEV_TTY:
            case devfs::DEV_CONSOLE:
            {
                sint64_t n = tty::read(buf, len);
                if (n < 0)
                    return n;                   // -EAGAIN
                *done = (uint64_t)n;
                return 0;
            }

            default:
                return -ENXIO;
        }
    }

    sint64_t dev_write(vnode* v, uint64_t off, const void* buf, uint64_t len,
                       uint64_t* done)
    {
        (void)off;
        *done = 0;

        switch (id_of(v))
        {
            case devfs::DEV_NULL:
            case devfs::DEV_ZERO:
                *done = len;                    // swallowed
                return 0;

            case devfs::DEV_TTY:
            case devfs::DEV_CONSOLE:
                *done = tty::write(buf, len);
                return 0;

            default:
                return -ENXIO;
        }
    }

    // Directory stream over the static table. Cookie:
    // 0 = ".", 1 = "..", 2 + i = devices[i]. Matches the layout the
    // getdents64 syscall layer expects from any FS (specials synthesized
    // here because devfs has no records on disk).
    sint64_t dev_readdir(vnode* dir, uint64_t* cookie, dirent_out* out,
                         bool* eof)
    {
        (void)dir;

        if (*cookie == 0)
        {
            out->ino = 1;
            out->type = DT_DIR;
            strncpy(out->name, ".", NAME_MAX);
            *cookie = 1;
            out->next_cookie = *cookie;
            *eof = false;
            return 0;
        }
        if (*cookie == 1)
        {
            out->ino = 1;
            out->type = DT_DIR;
            strncpy(out->name, "..", NAME_MAX);
            *cookie = 2;
            out->next_cookie = *cookie;
            *eof = false;
            return 0;
        }

        uint32_t i = (uint32_t)(*cookie - 2);
        if (i >= NDEV)
        {
            *eof = true;
            return 0;
        }

        out->ino = 2 + i;
        out->type = DT_CHR;
        strncpy(out->name, devices[i].name, NAME_MAX);
        out->name[NAME_MAX] = '\0';
        *cookie = *cookie + 1;
        out->next_cookie = *cookie;
        *eof = false;
        return 0;
    }

    sint64_t dev_getattr(vnode* v, struct stat* st)
    {
        memory::memset((uint8_t*)st, 0, sizeof(struct stat));
        st->st_dev = v->mnt ? v->mnt->dev_id : 0;
        st->st_nlink = 1;

        if (v == root_vn)
        {
            st->st_ino = 1;
            st->st_mode = S_IFDIR | 0755;
        }
        else
        {
            for (uint32_t i = 0; i < NDEV; i++)
                if (dev_vn[i] == v)
                {
                    st->st_ino = 2 + i;
                    st->st_mode = S_IFCHR | devices[i].mode;
                    // null=1,3 zero=1,5 tty=5,0 console=5,1 (Linux majors)
                    switch (devices[i].id)
                    {
                        case devfs::DEV_NULL:    st->st_rdev = (1ULL << 8) | 3; break;
                        case devfs::DEV_ZERO:    st->st_rdev = (1ULL << 8) | 5; break;
                        case devfs::DEV_TTY:     st->st_rdev = (5ULL << 8) | 0; break;
                        case devfs::DEV_CONSOLE: st->st_rdev = (5ULL << 8) | 1; break;
                    }
                    break;
                }
        }
        return 0;
    }

    sint64_t dev_ioctl(vnode* v, uint64_t request, uint64_t arg)
    {
        devfs::dev_id id = id_of(v);
        if (id != devfs::DEV_TTY && id != devfs::DEV_CONSOLE)
            return -ENOTTY;

        if (request == TCGETS)
        {
            // arg is a user pointer: the syscall layer already validated it
            // and passes the kernel-side bounce buffer here. devfs is the
            // one FS where ioctl receives a kernel pointer.
            tty::fill_termios((struct termios*)arg);
            return 0;
        }
        if (request == TIOCGWINSZ)
        {
            struct winsize* w = (struct winsize*)arg;
            w->ws_row = 25;     // TODO stage 4: real screen::rows()/cols()
            w->ws_col = 80;
            w->ws_xpixel = 0;
            w->ws_ypixel = 0;
            return 0;
        }
        return -ENOTTY;
    }

    bool dev_poll_ready(vnode* v)
    {
        devfs::dev_id id = id_of(v);
        if (id != devfs::DEV_TTY && id != devfs::DEV_CONSOLE)
            return true;
        // Ready when a canonical line is complete or the tty has keys to
        // assemble one from (read() consumes and assembles).
        return tty::has_input() || tty::line_len() > 0;
    }

    sint64_t dev_mount_fs(mount* m, void* arg, vnode** out_root)
    {
        (void)m;
        (void)arg;

        // No static constructors in a freestanding kernel: assemble the ops
        // table on first mount.
        if (!dev_ops.lookup)
        {
            dev_ops.lookup     = dev_lookup;
            dev_ops.create     = nullptr;
            dev_ops.mkdir      = nullptr;
            dev_ops.unlink     = nullptr;
            dev_ops.rmdir      = nullptr;
            dev_ops.rename     = nullptr;
            dev_ops.getparent  = dev_getparent;
            dev_ops.read       = dev_read;
            dev_ops.write      = dev_write;
            dev_ops.truncate   = nullptr;
            dev_ops.readdir    = dev_readdir;
            dev_ops.getattr    = dev_getattr;
            dev_ops.setattr    = nullptr;
            dev_ops.fsync      = nullptr;
            dev_ops.ioctl      = dev_ioctl;
            dev_ops.poll_ready = dev_poll_ready;
            dev_ops.release    = nullptr;
        }

        // Build the five vnodes once. They live until umount; devfs has no
        // on-disk state, so re-mounting rebuilds identical objects.
        sint64_t rc = 0;

        root_vn = vfs::get_cached(m, 1, vtype::DIR, &dev_ops, &rc);
        if (!root_vn)
            return rc;
        root_vn->mode = S_IFDIR | 0755;

        for (uint32_t i = 0; i < NDEV; i++)
        {
            dev_vn[i] = vfs::get_cached(m, 2 + i, vtype::CHR, &dev_ops, &rc);
            if (!dev_vn[i])
                return rc;
            dev_vn[i]->mode = S_IFCHR | devices[i].mode;
        }

        uart::printf("devfs: %u devices\n", NDEV);
        *out_root = root_vn;
        vfs::ref(root_vn);          // the mount holds its own reference
        return 0;
    }

    sint64_t dev_umount_fs(mount* m)
    {
        (void)m;
        root_vn = nullptr;
        for (uint32_t i = 0; i < NDEV; i++)
            dev_vn[i] = nullptr;
        return 0;
    }
}

namespace devfs
{
    vfs_fs fs =
    {
        "devfs",
        &dev_ops,
        dev_mount_fs,
        dev_umount_fs,
    };
}

