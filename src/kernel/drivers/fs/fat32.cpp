// Legacy path-based FAT32 API, now a thin shim over the VFS driver
// (src/kernel/fs/fat32/). The real implementation moved there in stage 3.3;
// this file exists only so the console, SYSX_* syscalls and process::run
// keep working until stage 3.7 ports them to vfs_*/fds and deletes it.
//
// Differences from the pre-3.3 driver that callers must know:
//   * paths are '/'-separated (backslash support is gone);
//   * names are matched through the VFS driver, i.e. LFN-aware and
//     case-insensitive;
//   * ls() fills the 11-byte name field with the record's display name
//     truncated to 8.3 shape - fine while the image only holds 8.3 names.
//     (cmd_ls is ported to readdir in 3.7.)

#include "../../../include/drivers/fs/fat32.h"
#include "../../../include/fs/vfs.h"
#include "../../../include/fs/fat32fs.h"
#include "../../../include/dev/blkdev.h"
#include "../../../include/dev/bcache.h"
#include "../../../include/mm/heap.h"
#include "../../../include/mm/memory.h"
#include "../../../include/stdlib/string.h"
#include "../../../include/drivers/uart.h"
#include "../../../sdk/include/abi/errno.h"
#include "../../../sdk/include/abi/stat.h"
#include "../../../sdk/include/abi/dirent.h"

// ---------------------------------------------------------------------------
// Formatting helpers (unchanged public behaviour; used by cmd_ls and the
// SYSX_READ_DIR/SYSX_STAT_FILE syscalls until 3.7 replaces them).
// ---------------------------------------------------------------------------

uint32_t format_83_name(const uint8_t* raw, char* out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = (char)raw[i];
    if (raw[8] != ' ' || raw[9] != ' ' || raw[10] != ' ')
    {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = (char)raw[i];
    }
    out[n] = '\0';
    return n;
}

// FAT date: bits 15-9 = year-1980, bits 8-5 = month, bits 4-0 = day
// FAT time: bits 15-11 = hours, bits 10-5 = minutes, bits 4-0 = seconds/2
void format_datetime(uint16_t date, uint16_t time, char* out)
{
    if (date == 0 && time == 0)
    {
        strcpy(out, "                ");
        return;
    }

    uint16_t day   = date & 0x1F;
    uint16_t month = (date >> 5) & 0x0F;
    uint16_t year  = ((date >> 9) & 0x7F) + 1980;
    uint16_t hour  = (time >> 11) & 0x1F;
    uint16_t min   = (time >> 5) & 0x3F;

    out[0]  = (char)('0' + day / 10);
    out[1]  = (char)('0' + day % 10);
    out[2]  = '.';
    out[3]  = (char)('0' + month / 10);
    out[4]  = (char)('0' + month % 10);
    out[5]  = '.';
    out[6]  = (char)('0' + (year / 1000) % 10);
    out[7]  = (char)('0' + (year / 100) % 10);
    out[8]  = (char)('0' + (year / 10) % 10);
    out[9]  = (char)('0' + year % 10);
    out[10] = ' ';
    out[11] = (char)('0' + hour / 10);
    out[12] = (char)('0' + hour % 10);
    out[13] = ':';
    out[14] = (char)('0' + min / 10);
    out[15] = (char)('0' + min % 10);
    out[16] = '\0';
}

// ---------------------------------------------------------------------------
// Shim
// ---------------------------------------------------------------------------

namespace
{
    // The mount this shim operates on: the root mount, when it is FAT.
    // `struct mount` spelled out: the fat32::mount function shadows the type
    // name inside this namespace.
    struct mount* fat_mount()
    {
        struct mount* m = vfs::root_mount();
        if (m && m->fs == &fat32fs::fs)
            return m;
        return nullptr;
    }

    void epoch_to_fat_dt(uint64_t epoch, uint16_t* date, uint16_t* time)
    {
        fatdir::epoch_to_fat(epoch, date, time);
    }

    // Fill a legacy fat32_dir_entry from a vnode (fields the two legacy
    // consumers actually read: file_size, attr, write_date/time).
    void entry_from_vnode(vnode* v, fat32_dir_entry* out)
    {
        memory::memset((uint8_t*)out, 0, sizeof(fat32_dir_entry));

        struct stat st;
        v->ops->getattr(v, &st);

        out->file_size = (uint32_t)(st.st_size > 0xFFFFFFFFLL
                                    ? 0xFFFFFFFF : st.st_size);
        if (v->type == vtype::DIR)
            out->attr = FAT32_ATTR_DIRECTORY;
        else
        {
            out->attr = FAT32_ATTR_ARCHIVE;
            if (!(st.st_mode & S_IWUSR))
                out->attr |= FAT32_ATTR_READ_ONLY;
        }
        epoch_to_fat_dt((uint64_t)st.st_mtim.tv_sec,
                        &out->write_date, &out->write_time);
        epoch_to_fat_dt((uint64_t)st.st_ctim.tv_sec,
                        &out->create_date, &out->create_time);
    }

    // Pack a display name into the 11-byte 8.3 field (transitional, see the
    // file header).
    void pack_name(const char* name, uint8_t* out11)
    {
        memory::memset(out11, ' ', 11);

        uint32_t len = strlen(name);
        uint32_t dot = 0xFFFFFFFF;
        for (uint32_t i = 0; i < len; i++)
            if (name[i] == '.' && i != 0)
                dot = i;

        uint32_t blen = (dot == 0xFFFFFFFF) ? len : dot;
        const char* ext = (dot == 0xFFFFFFFF) ? nullptr : name + dot + 1;
        uint32_t elen = (dot == 0xFFFFFFFF) ? 0 : len - dot - 1;

        for (uint32_t i = 0; i < blen && i < 8; i++)
        {
            char c = name[i];
            out11[i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
        for (uint32_t i = 0; ext && i < elen && i < 3; i++)
        {
            char c = ext[i];
            out11[8 + i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
    }
}

namespace fat32
{
    bool mount(blkdev* volume)
    {
        if (!volume)
            return false;

        // Remount: drop the previous FAT root first.
        if (fat_mount())
            umount();

        if (vfs::root_mount())
        {
            uart::printf("fat32-shim: a non-FAT root is already mounted\n");
            return false;
        }

        if (vfs::mount_at(nullptr, volume->name, &fat32fs::fs, volume) != 0)
            return false;

        // The console's cwd starts at the new root.
        struct mount* m = vfs::root_mount();
        if (m)
        {
            vfs::ref(m->root);
            vfs::set_cwd(m->root);
        }
        return true;
    }

    void umount()
    {
        struct mount* m = fat_mount();
        if (!m)
            return;

        // The cwd may point into this FS: park it on the root vnode so the
        // vnode sweep inside vfs::umount can free everything.
        vfs::set_cwd(nullptr);
        vfs::umount(m);
    }

    bool is_mounted()
    {
        return fat_mount() != nullptr;
    }

    bool resolve_path_pub(const char* path, fat32_dir_entry* out_entry)
    {
        vnode* v = nullptr;
        if (vfs::lookup(path, vfs::cwd(), &v, false) != 0)
            return false;

        entry_from_vnode(v, out_entry);
        // The legacy struct's name field is unused by both consumers, but
        // fill it from the path's last component for consistency.
        uint32_t len = strlen(path);
        uint32_t cut = len;
        while (cut > 0 && path[cut - 1] != '/')
            cut--;
        pack_name(path + cut, out_entry->name);

        vfs::unref(v);
        return true;
    }

    uint32_t ls(const char* path, fat32_dir_entry* entries, uint32_t max_entries)
    {
        const char* p = (path && path[0]) ? path : ".";

        vnode* dir = nullptr;
        if (vfs::lookup(p, vfs::cwd(), &dir, true) != 0)
            return 0;

        uint64_t cookie = 0;
        uint32_t count = 0;

        // "." and ".." first, like every directory stream.
        if (max_entries >= 1)
        {
            memory::memset((uint8_t*)&entries[0], 0, sizeof(fat32_dir_entry));
            entries[0].name[0] = '.';
            entries[0].attr = FAT32_ATTR_DIRECTORY;
            count++;
        }
        if (max_entries >= 2)
        {
            memory::memset((uint8_t*)&entries[1], 0, sizeof(fat32_dir_entry));
            entries[1].name[0] = '.';
            entries[1].name[1] = '.';
            entries[1].attr = FAT32_ATTR_DIRECTORY;
            count++;
        }

        while (count < max_entries)
        {
            dirent_out d;
            bool eof = false;
            sint64_t rc = dir->ops->readdir(dir, &cookie, &d, &eof);
            if (rc != 0 || eof)
                break;

            fat32_dir_entry* e = &entries[count++];
            memory::memset((uint8_t*)e, 0, sizeof(fat32_dir_entry));
            pack_name(d.name, e->name);
            e->attr = (d.type == DT_DIR) ? FAT32_ATTR_DIRECTORY
                                         : FAT32_ATTR_ARCHIVE;

            // Size/time: resolve the child vnode (cached, so cheap).
            vnode* child = nullptr;
            if (vfs::lookup(d.name, dir, &child, false) == 0)
            {
                struct stat st;
                child->ops->getattr(child, &st);
                e->file_size = (uint32_t)st.st_size;
                if (!(st.st_mode & S_IWUSR))
                    e->attr |= FAT32_ATTR_READ_ONLY;
                epoch_to_fat_dt((uint64_t)st.st_mtim.tv_sec,
                                &e->write_date, &e->write_time);
                vfs::unref(child);
            }
        }

        vfs::unref(dir);
        return count;
    }

    uint32_t read_file(const char* path, uint8_t* buffer, uint32_t max_size)
    {
        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, vfs::cwd(), &v, false);
        if (rc != 0)
            return (uint32_t)-1;

        if (v->type != vtype::REG)
        {
            vfs::unref(v);
            return (uint32_t)-1;
        }

        uint64_t done = 0;
        rc = v->ops->read(v, 0, buffer, max_size, &done);
        vfs::unref(v);

        return rc == 0 ? (uint32_t)done : (uint32_t)-1;
    }

    uint32_t write_file(const char* path, const uint8_t* data, uint32_t size)
    {
        // Legacy semantics: create-or-replace the whole file.
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, vfs::cwd(), &parent, name);
        if (rc != 0)
            return (uint32_t)-1;

        vnode* v = nullptr;
        rc = parent->ops->lookup(parent, name, &v);
        if (rc == 0 && v)
        {
            if (v->type != vtype::REG)
            {
                vfs::unref(v);
                vfs::unref(parent);
                return (uint32_t)-1;
            }
            // Truncate to zero, then write.
            rc = v->ops->truncate(v, 0);
            if (rc != 0)
            {
                vfs::unref(v);
                vfs::unref(parent);
                return (uint32_t)-1;
            }
        }
        else
        {
            rc = parent->ops->create(parent, name, 0644, &v);
            if (rc != 0)
            {
                vfs::unref(parent);
                return (uint32_t)-1;
            }
        }

        uint64_t done = 0;
        rc = size ? v->ops->write(v, 0, data, size, &done) : 0;
        vfs::unref(v);
        vfs::unref(parent);

        return rc == 0 ? (uint32_t)done : (uint32_t)-1;
    }

    bool mkdir(const char* path)
    {
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, vfs::cwd(), &parent, name);
        if (rc != 0)
            return false;

        rc = parent->ops->mkdir(parent, name, 0755);
        vfs::unref(parent);
        return rc == 0;
    }

    bool remove(const char* path)
    {
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(path, vfs::cwd(), &parent, name);
        if (rc != 0)
            return false;

        // Legacy remove() handled files and directories with one call.
        vnode* target = nullptr;
        rc = parent->ops->lookup(parent, name, &target);
        if (rc != 0)
        {
            vfs::unref(parent);
            return false;
        }
        bool is_dir = (target->type == vtype::DIR);
        vfs::unref(target);

        rc = is_dir ? parent->ops->rmdir(parent, name)
                    : parent->ops->unlink(parent, name);
        vfs::unref(parent);
        return rc == 0;
    }

    bool rename(const char* old_path, const char* new_name)
    {
        // Legacy semantics: new_name is a bare name in the same directory.
        vnode* parent = nullptr;
        char name[NAME_MAX + 1];
        sint64_t rc = vfs::lookup_parent(old_path, vfs::cwd(), &parent, name);
        if (rc != 0)
            return false;

        for (const char* p = new_name; *p; p++)
            if (*p == '/')
            {
                vfs::unref(parent);
                return false;
            }

        rc = parent->ops->rename(parent, name, parent, new_name, 0);
        vfs::unref(parent);
        return rc == 0;
    }

    bool copy(const char* src_path, const char* dst_path)
    {
        vnode* src = nullptr;
        if (vfs::lookup(src_path, vfs::cwd(), &src, false) != 0)
            return false;
        if (src->type != vtype::REG)
        {
            vfs::unref(src);
            return false;
        }

        struct stat st;
        src->ops->getattr(src, &st);
        uint64_t size = (uint64_t)st.st_size;

        uint8_t* buf = (uint8_t*)kmalloc(size ? size : 1);
        if (!buf)
        {
            vfs::unref(src);
            return false;
        }

        uint64_t done = 0;
        bool ok = size == 0 || src->ops->read(src, 0, buf, size, &done) == 0;
        vfs::unref(src);
        if (!ok || done != size)
        {
            kfree(buf);
            return false;
        }

        uint32_t written = write_file(dst_path, buf, (uint32_t)size);
        kfree(buf);
        return written == (uint32_t)size;
    }

    bool set_cwd(const char* path)
    {
        vnode* v = nullptr;
        sint64_t rc = vfs::lookup(path, vfs::cwd(), &v, true);
        if (rc != 0)
            return false;
        vfs::set_cwd(v);            // takes the reference
        return true;
    }

    // Returns a static buffer; the prompt and `cd` print it immediately.
    const char* cwd_path()
    {
        static char buf[PATH_MAX];
        if (vfs::cwd_path(buf, sizeof(buf)) != 0)
        {
            buf[0] = '/';
            buf[1] = '\0';
        }
        return buf;
    }
}
