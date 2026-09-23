// src/kernel/drivers/commands.cpp
#include "../../include/drivers/commands.h"
#include "../../include/dev/blkdev.h"
#include "../../include/dev/bcache.h"
#include "../../include/fs/vfs.h"
#include "../../include/fs/fat32fs.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/usb/xhci.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/mm/pmm.h"
#include "../../include/drivers/pit.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/rtc.h"
#include "../../include/cpu/process.h"
#include "../../sdk/include/abi/process.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/stat.h"
#include "../../sdk/include/abi/dirent.h"
#include "../../sdk/include/abi/time.h"

namespace
{
    // Format epoch seconds as DD.MM.YYYY HH:MM (UTC) into out[17].
    void format_epoch(uint64_t epoch, char* out)
    {
        if (epoch == 0)
        {
            strcpy(out, "                ");
            return;
        }
        uint32_t secs = (uint32_t)(epoch % 86400);
        sint64_t days = (sint64_t)(epoch / 86400);

        sint64_t z = days + 719468;
        sint64_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        sint64_t y = (sint64_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp + (mp < 10 ? 3u : (uint32_t)-9);
        y += (m <= 2);

        uint32_t hh = secs / 3600, mm = (secs / 60) % 60;
        out[0]  = (char)('0' + d / 10);
        out[1]  = (char)('0' + d % 10);
        out[2]  = '.';
        out[3]  = (char)('0' + m / 10);
        out[4]  = (char)('0' + m % 10);
        out[5]  = '.';
        out[6]  = (char)('0' + (y / 1000) % 10);
        out[7]  = (char)('0' + (y / 100) % 10);
        out[8]  = (char)('0' + (y / 10) % 10);
        out[9]  = (char)('0' + y % 10);
        out[10] = ' ';
        out[11] = (char)('0' + hh / 10);
        out[12] = (char)('0' + hh % 10);
        out[13] = ':';
        out[14] = (char)('0' + mm / 10);
        out[15] = (char)('0' + mm % 10);
        out[16] = '\0';
    }
}

// Built-in commands

static void cmd_help(int argc, const char** argv)
{
    uint32_t cols = screen::cols();

    uint32_t col_width = 16;
    uint32_t num_cols = cols / col_width;
    if (num_cols < 1)
        num_cols = 1;

    uint32_t count = console::command_count();

    screen::printf("\n\r");
    for (uint32_t i = 0; i < count; i++)
    {
        if (i % num_cols == 0 && i > 0)
            screen::printf("\n\r");

        const char* name = console::command_name(i);
        screen::printf(" %s", name);

        uint32_t name_len = strlen(name);
        uint32_t pad = col_width - name_len - 1;
        for (uint32_t p = 0; p < pad; p++)
            screen::printf(" ");
    }
}

static void cmd_cls(int argc, const char** argv)
{
    screen::clear();
    screen::show_cursor();
}

static void cmd_cpuid(int argc, const char** argv)
{
    char name[64];
    cpuid::get_cpu_name(name);

    CPUTopology topo;
    cpuid::get_cpu_topology(&topo);

    CacheInfo cache;
    cpuid::get_cache_info(&cache);

    screen::printf("\n\r");
    screen::printf("\n\r CPU:            %s", name);
    screen::printf("\n\r Base freq:      %u MHz", cpuid::get_base_freq());
    screen::printf("\n\r Max freq:       %u MHz", cpuid::get_max_freq());
    screen::printf("\n\r Bus freq:       %u MHz", cpuid::get_bus_freq());
    screen::printf("\n\r Logical cores:  %u", topo.logical_cores);
    screen::printf("\n\r Physical cores: %u", topo.physical_cores);
    screen::printf("\n\r Sockets:        %u", topo.packages);
    screen::printf("\n\r Hyperthreading: %s", topo.hyperthreading ? "Yes" : "No");
    screen::printf("\n\r L1 cache:       %u KB", cache.l1d_size + cache.l1i_size);
    screen::printf("\n\r L2 cache:       %u KB", cache.l2_size);
    screen::printf("\n\r L3 cache:       %u KB", cache.l3_size);
}

static void cmd_lspci(int argc, const char** argv)
{
    uint32_t count = pci::device_count();
    screen::printf("\n\r");
    screen::printf("\n\r PCI devices found: %u", count);
    screen::printf("\n\r");

    for (uint32_t i = 0; i < count; i++)
    {
        PCIDevice* d = pci::get_by_id(i);

        char vid[5], did[5];
        hex_to_str(d->vendor_id, vid, 4);
        hex_to_str(d->device_id, did, 4);

        screen::printf("\n\r  %u:%u.%u  0x%s:0x%s  %s",
            (uint32_t)d->bus, (uint32_t)d->device, (uint32_t)d->function,
            vid, did,
            pci::class_name(d->class_code));
    }
}

// mount <device> <dir>: mount a FAT32 volume over an existing directory.
static void cmd_mount(int argc, const char** argv)
{
    if (argc == 1)
    {
        // No arguments: list the mount table.
        screen::printf("\n\r");
        for (uint32_t i = 0; vfs::mount_count_get(i); i++)
        {
            mount* m = vfs::mount_count_get(i);
            if (m->point)
            {
                char buf[PATH_MAX];
                if (vfs::get_path(m->point, buf, sizeof(buf), nullptr) == 0)
                    screen::printf("  %s on %s\n\r", m->devname, buf);
            }
            else
                screen::printf("  %s on /\n\r", m->devname);
        }
        return;
    }

    if (argc < 3)
    {
        screen::printf("\n\rUsage: mount <device> <dir>   (lsblk lists devices)");
        return;
    }

    blkdev* dev = block::find(argv[1]);
    if (!dev)
    {
        screen::printf("\n\rNo such block device: %s", argv[1]);
        return;
    }

    vnode* point = nullptr;
    sint64_t rc = vfs::lookup(argv[2], vfs::cwd(), &point, true);
    if (rc != 0)
    {
        screen::printf("\n\rMount point not found: %s", argv[2]);
        return;
    }

    rc = vfs::mount_at(point, dev->name, &fat32fs::fs, dev);  // takes the ref
    if (rc != 0)
        screen::printf("\n\rMount failed (%d): no FAT32 volume on %s",
                       (int)rc, dev->name);
    else
        screen::printf("\n\r%s mounted on %s", dev->name, argv[2]);
}

static void cmd_umount(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: umount <dir>");
        return;
    }

    vnode* point = nullptr;
    sint64_t rc = vfs::lookup(argv[1], vfs::cwd(), &point, true);
    if (rc != 0)
    {
        screen::printf("\n\rNot a directory: %s", argv[1]);
        return;
    }

    // namei descends *into* a mount when it walks onto its point, so the
    // vnode we get back for "/dev" is the mounted FS's root, not the
    // directory it covers. Match on the root; keep the point comparison as
    // a fallback for the case where the lookup did not cross (no mount).
    mount* found = nullptr;
    for (uint32_t i = 0; vfs::mount_count_get(i); i++)
    {
        mount* m = vfs::mount_count_get(i);
        if (m->root == point || m->point == point)
        {
            found = m;
            break;
        }
    }
    vfs::unref(point);

    if (!found)
    {
        screen::printf("\n\rNothing is mounted there");
        uart::printf("umount: nothing mounted at %s\n", argv[1]);
        return;
    }
    if (!found->point)
    {
        screen::printf("\n\rCannot umount the root filesystem");
        uart::printf("umount: refused, %s is the root filesystem\n", argv[1]);
        return;
    }

    rc = vfs::umount(found);
    if (rc == 0)
    {
        screen::printf("\n\rUnmounted");
        uart::printf("umount: ok %s\n", argv[1]);
    }
    else if (rc == -EBUSY)
    {
        screen::printf("\n\rBusy: something still has it open");
        uart::printf("umount: busy %s\n", argv[1]);
    }
    else
    {
        screen::printf("\n\rUnmount failed (%d)", (int)rc);
        uart::printf("umount: failed %s rc=%d\n", argv[1], (int)rc);
    }
}

static void cmd_sync(int argc, const char** argv)
{
    (void)argc; (void)argv;
    sint64_t rc = vfs::sync_all();
    if (rc == 0)
    {
        screen::printf("\n\rSynchronized");
        uart::printf("sync: ok\n");
    }
    else
    {
        screen::printf("\n\rSync failed (%d)", (int)rc);
        uart::printf("sync: failed %d\n", (int)rc);
    }
}

static void cmd_ls(int argc, const char** argv)
{
    const char* path = argc > 1 ? argv[1] : ".";

    vnode* dir = nullptr;
    sint64_t rc = vfs::lookup(path, vfs::cwd(), &dir, true);
    if (rc != 0)
    {
        screen::printf("\n\rDirectory not found: %s", path);
        return;
    }

    uint64_t cookie = 0;
    screen::printf("\n\r");
    for (;;)
    {
        dirent_out d;
        bool eof = false;
        rc = dir->ops->readdir(dir, &cookie, &d, &eof);
        if (rc != 0)
        {
            screen::printf("read error (%d)\n\r", (int)rc);
            break;
        }
        if (eof)
            break;

        vnode* child = nullptr;
        rc = vfs::lookup(d.name, dir, &child, false);
        if (rc == 0 && child)
        {
            struct stat st;
            child->ops->getattr(child, &st);

            char dt[17];
            format_epoch((uint64_t)st.st_mtim.tv_sec, dt);

            if (st.st_mode & S_IFDIR)
                screen::printf("  %s       <DIR>  %s\n\r", dt, d.name);
            else
                screen::printf("  %s  %10u  %s\n\r", dt,
                               (uint32_t)st.st_size, d.name);
            vfs::unref(child);
        }
        else
        {
            screen::printf("                              %s\n\r", d.name);
        }
    }
    vfs::unref(dir);
}

// Copy a regular file through the VFS in 32 KiB chunks.
static void cmd_cp(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: cp <source> <destination>");
        return;
    }

    vnode* src = nullptr;
    sint64_t rc = vfs::lookup(argv[1], vfs::cwd(), &src, false);
    if (rc != 0 || src->type != vtype::REG)
    {
        if (src) vfs::unref(src);
        screen::printf("\n\rSource is not a file: %s", argv[1]);
        return;
    }

    // Destination: create in its parent directory.
    vnode* parent = nullptr;
    char name[NAME_MAX + 1];
    rc = vfs::lookup_parent(argv[2], vfs::cwd(), &parent, name);
    if (rc != 0)
    {
        vfs::unref(src);
        screen::printf("\n\rDestination not found: %s", argv[2]);
        return;
    }

    vnode* dst = nullptr;
    rc = parent->ops->lookup(parent, name, &dst);
    if (rc == 0 && dst)
    {
        if (dst->type != vtype::REG)
        {
            vfs::unref(dst);
            vfs::unref(parent);
            vfs::unref(src);
            screen::printf("\n\rDestination is not a file");
            return;
        }
        rc = dst->ops->truncate(dst, 0);
    }
    else
    {
        rc = parent->ops->create(parent, name, 0644, &dst);
    }
    vfs::unref(parent);
    if (rc != 0)
    {
        vfs::unref(src);
        screen::printf("\n\rCannot create %s (%d)", argv[2], (int)rc);
        return;
    }

    const uint32_t CHUNK = 32 * 1024;
    uint8_t* buf = (uint8_t*)kmalloc(CHUNK);
    bool ok = buf != nullptr;
    uint64_t off = 0, woff = 0;

    while (ok && off < src->size)
    {
        uint64_t want = src->size - off;
        if (want > CHUNK) want = CHUNK;

        uint64_t done = 0;
        rc = src->ops->read(src, off, buf, want, &done);
        if (rc != 0 || done == 0) { ok = false; break; }

        uint64_t wdone = 0;
        rc = dst->ops->write(dst, woff, buf, done, &wdone);
        if (rc != 0 || wdone != done) { ok = false; break; }

        off += done;
        woff += wdone;
    }
    if (buf) kfree(buf);

    if (ok)
    {
        dst->ops->fsync(dst);
        screen::printf("\n\rCopied %s -> %s", argv[1], argv[2]);
    }
    else
        screen::printf("\n\rCopy failed (%d)", (int)rc);

    vfs::unref(src);
    vfs::unref(dst);
}

static void cmd_mv(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: mv <source> <destination>");
        return;
    }

    vnode* od = nullptr, *nd = nullptr;
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];

    sint64_t rc = vfs::lookup_parent(argv[1], vfs::cwd(), &od, oname);
    if (rc == 0)
        rc = vfs::lookup_parent(argv[2], vfs::cwd(), &nd, nname);
    if (rc == 0)
        rc = od->ops->rename(od, oname, nd, nname, 0);

    if (od) vfs::unref(od);
    if (nd) vfs::unref(nd);

    screen::printf("\n\r");
    if (rc == 0)
        screen::printf("Moved %s -> %s", argv[1], argv[2]);
    else
        screen::printf("Move failed (%d)", (int)rc);
}

static bool is_printable(uint8_t c)
{
    return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
}

static void cmd_cat(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: cat <filename>");
        return;
    }

    const uint32_t max_size = 512;
    uint8_t* buf = (uint8_t*)kmalloc(max_size);
    if (!buf)
    {
        screen::printf("\n\rOut of memory");
        return;
    }

    vnode* v = nullptr;
    sint64_t vrc = vfs::lookup(argv[1], vfs::cwd(), &v, false);
    if (vrc != 0 || v->type != vtype::REG)
    {
        if (v) vfs::unref(v);
        screen::printf("\n\rFile not found: %s", argv[1]);
        kfree(buf);
        return;
    }
    uint64_t done = 0;
    vrc = v->ops->read(v, 0, buf, max_size, &done);
    vfs::unref(v);
    if (vrc != 0)
    {
        screen::printf("\n\rRead error (%d)", (int)vrc);
        kfree(buf);
        return;
    }
    uint32_t n = (uint32_t)done;
    if (n == 0)
    {
        screen::printf("\n\rFile not found or read error");
        kfree(buf);
        return;
    }

    // Detect binary content
    bool binary = false;
    for (uint32_t i = 0; i < n; i++)
    {
        if (!is_printable(buf[i]))
        {
            binary = true;
            break;
        }
    }

    if (binary)
    {
        screen::printf("\n\rBinary file (%u bytes), use xxd to view", n);
        kfree(buf);
        return;
    }

    screen::printf("\n\r");
    buf[n] = 0;
    screen::printf("%s", (char*)buf);

    if (n == max_size)
    {
        screen::printf("\n\r[truncated at %u bytes]", max_size);
    }
    else 
    {
        screen::printf("\n\r%u bytes", n);
    }

    kfree(buf);
}

static void cmd_xxd(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: xxd <filename>");
        return;
    }

    const uint32_t max_size = 512;
    uint8_t* buf = (uint8_t*)kmalloc(max_size);
    if (!buf)
    {
        screen::printf("\n\rOut of memory");
        return;
    }

    vnode* v = nullptr;
    sint64_t vrc = vfs::lookup(argv[1], vfs::cwd(), &v, false);
    if (vrc != 0 || v->type != vtype::REG)
    {
        if (v) vfs::unref(v);
        screen::printf("\n\rFile not found: %s", argv[1]);
        kfree(buf);
        return;
    }
    uint64_t done = 0;
    vrc = v->ops->read(v, 0, buf, max_size, &done);
    vfs::unref(v);
    if (vrc != 0)
    {
        screen::printf("\n\rRead error (%d)", (int)vrc);
        kfree(buf);
        return;
    }
    uint32_t n = (uint32_t)done;
    if (n == 0)
    {
        screen::printf("\n\rFile not found or read error");
        kfree(buf);
        return;
    }

    screen::printf("\n\r");

    const char* hex = "0123456789ABCDEF";

    for (uint32_t off = 0; off < n; off += 16)
    {
        // Offset (8 hex digits)
        char addr[9];
        for (int i = 7; i >= 0; i--)
            addr[i] = hex[(off >> ((7 - i) * 4)) & 0xF];
        addr[8] = '\0';
        screen::printf("%s  ", addr);

        // Hex bytes
        for (uint32_t i = 0; i < 16; i++)
        {
            if (off + i < n)
            {
                uint8_t b = buf[off + i];
                screen::printf("%c%c ", hex[b >> 4], hex[b & 0xF]);
            }
            else
            {
                screen::printf("   ");
            }

            if (i == 7) screen::printf(" ");
        }

        // ASCII column
        screen::printf(" |");
        for (uint32_t i = 0; i < 16 && off + i < n; i++)
        {
            uint8_t b = buf[off + i];
            if (b >= 0x20 && b <= 0x7E)
                screen::printf("%c", b);
            else
                screen::printf(".");
        }
        screen::printf("|\n\r");
    }

    if (n == max_size) 
    {
        screen::printf("[truncated at %u bytes]\n\r", max_size);
    }
    else 
    {
        screen::printf("\n\r%u bytes", n);
    }   

    kfree(buf);
}

static void cmd_write(int argc, const char** argv)
{
    if (argc < 3)
    {
        screen::printf("\n\rUsage: write <filename> <text...>");
        return;
    }

    // Concatenate all args after filename
    char data[512];
    uint32_t pos = 0;
    for (int i = 2; i < argc && pos < 510; i++)
    {
        if (i > 2 && pos < 510)
            data[pos++] = ' ';

        uint32_t len = strlen(argv[i]);
        for (uint32_t j = 0; j < len && pos < 510; j++)
            data[pos++] = argv[i][j];
    }
    data[pos] = '\0';

    screen::printf("\n\r");

    vnode* parent = nullptr;
    char name[NAME_MAX + 1];
    sint64_t rc = vfs::lookup_parent(argv[1], vfs::cwd(), &parent, name);
    if (rc != 0)
    {
        screen::printf("Path not found: %s (%d)", argv[1], (int)rc);
        return;
    }

    vnode* v = nullptr;
    rc = parent->ops->lookup(parent, name, &v);
    if (rc == 0 && v)
    {
        if (v->type != vtype::REG)
        {
            vfs::unref(v);
            vfs::unref(parent);
            screen::printf("Not a file: %s", argv[1]);
            return;
        }
        v->ops->truncate(v, 0);
    }
    else
        rc = parent->ops->create(parent, name, 0644, &v);
    vfs::unref(parent);

    if (rc != 0)
    {
        screen::printf("Cannot create %s (%d)", argv[1], (int)rc);
        return;
    }

    uint64_t wdone = 0;
    rc = pos ? v->ops->write(v, 0, data, pos, &wdone) : 0;
    vfs::unref(v);

    if (rc == 0)
        screen::printf("%u bytes written to %s", (uint32_t)wdone, argv[1]);
    else
        screen::printf("Write failed (%d)", (int)rc);
}

static void cmd_mkdir(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: mkdir <dirname>");
        return;
    }

    vnode* parent = nullptr;
    char name[NAME_MAX + 1];
    sint64_t rc = vfs::lookup_parent(argv[1], vfs::cwd(), &parent, name);
    if (rc == 0)
        rc = parent->ops->mkdir(parent, name, 0755);
    if (parent)
        vfs::unref(parent);

    screen::printf("\n\r");
    if (rc == 0)
        screen::printf("Directory created: %s", argv[1]);
    else
        screen::printf("mkdir failed (%d)", (int)rc);
}

// rm <path>: unlink a file. rm -r is not supported (use rmdir for empty
// directories).
static void cmd_rm(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: rm <file> | rmdir <dir>");
        return;
    }

    vnode* parent = nullptr;
    char name[NAME_MAX + 1];
    sint64_t rc = vfs::lookup_parent(argv[1], vfs::cwd(), &parent, name);
    if (rc == 0)
        rc = parent->ops->unlink(parent, name);
    if (parent)
        vfs::unref(parent);

    screen::printf("\n\r");
    if (rc == 0)
        screen::printf("Removed: %s", argv[1]);
    else
        screen::printf("Remove failed (%d)", (int)rc);
}

static void cmd_rmdir(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: rmdir <dir>");
        return;
    }

    vnode* parent = nullptr;
    char name[NAME_MAX + 1];
    sint64_t rc = vfs::lookup_parent(argv[1], vfs::cwd(), &parent, name);
    if (rc == 0)
        rc = parent->ops->rmdir(parent, name);
    if (parent)
        vfs::unref(parent);

    screen::printf("\n\r");
    if (rc == 0)
        screen::printf("Removed: %s", argv[1]);
    else if (rc == -ENOTEMPTY)
        screen::printf("Directory not empty: %s", argv[1]);
    else
        screen::printf("rmdir failed (%d)", (int)rc);
}

static void cmd_pwd(int argc, const char** argv)
{
    (void)argc; (void)argv;
    char buf[PATH_MAX];
    sint64_t rc = vfs::cwd_path(buf, sizeof(buf));
    screen::printf("\n\r%s", rc == 0 ? buf : "?");
}

// dmesg: the kernel boot log. Everything the drivers report goes to the
// serial line, which no laptop has, so keep a copy on screen too.
static void cmd_dmesg(int argc, const char** argv)
{
    static char buf[UART_LOG_SIZE + 1];
    uint32_t len = uart::log_read(buf, UART_LOG_SIZE);
    buf[len] = '\0';

    screen::printf("\n\r");
    for (uint32_t i = 0; i < len; i++)
    {
        // The log uses bare newlines; the console wants CR with them.
        if (buf[i] == '\n')
            screen::printf("\n\r");
        else
            screen::printf("%c", buf[i]);
    }
}

// usbports: the raw root-port state of the active controller. The one thing
// worth photographing when a machine enumerates nothing: it separates "no
// controller", "port unpowered", "nothing plugged in" and "device present but
// enumeration failed".
static void cmd_usbports(int argc, const char** argv)
{
    uint8_t ports = usb::get_port_count();
    screen::printf("\n\r");
    screen::printf("\n\r Root ports: %u   context entry: %u bytes",
                   (uint32_t)ports, usb::get_context_entry_size());

    if (ports == 0)
    {
        screen::printf("\n\r No controller running");
        return;
    }

    for (uint8_t i = 0; i < ports; i++)
    {
        uint32_t raw = usb::get_port_status(i);
        char hex[9];
        hex_to_str(raw, hex, 8);

        screen::printf("\n\r  [%u] %s  0x%s  ccs=%u ped=%u pp=%u pr=%u pls=%u spd=%u",
            (uint32_t)i,
            usb::port_is_usb3(i) ? "usb3" : "usb2",
            hex,
            raw & 1, (raw >> 1) & 1, (raw >> 9) & 1, (raw >> 4) & 1,
            (raw >> 5) & 0xF, (raw >> 10) & 0xF);
    }
}

static void cmd_lsusb(int argc, const char** argv)
{
    uint8_t count = usb::get_device_count();
    screen::printf("\n\r");
    screen::printf("\n\r USB devices: %u", (uint32_t)count);
    screen::printf("\n\r");

    if (count == 0)
    {
        screen::printf("\n\r No devices found");
        return;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        usb_device_info info;
        if (usb::get_device_info(i, &info) != USB_OK)
            continue;

        char vid[5], pid[5];
        hex_to_str(info.vendor_id, vid, 4);
        hex_to_str(info.product_id, pid, 4);

        screen::printf("\n\r  [%u] %s:%s  slot=%u port=%u  %s",
            (uint32_t)i, vid, pid,
            (uint32_t)info.slot_id, 
            (uint32_t)info.port_index, 
            usb::get_usb_speed_str(info.port_speed));
        screen::printf("\n\r      class=%s  %s\n\r", 
            usb::get_usb_class_name(info.device_class),
            info.is_mass_storage ? "[mass storage]" : "");
    }
}

static void cmd_usbinfo(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: usbinfo <index>");
        return;
    }

    uint8_t index = 0;
    for (int i = 0; argv[1][i] != '\0'; i++)
    {
        if (argv[1][i] < '0' || argv[1][i] > '9')
        {
            screen::printf("\n\rInvalid index");
            return;
        }
        index = index * 10 + (argv[1][i] - '0');
    }

    usb_device_info info;
    if (usb::get_device_info(index, &info) != USB_OK)
    {
        screen::printf("\n\rDevice %u not found", (uint32_t)index);
        return;
    }

    char vid[5], pid[5];
    hex_to_str(info.vendor_id, vid, 4);
    hex_to_str(info.product_id, pid, 4);

    char bcd[5];
    hex_to_str(info.bcd_usb, bcd, 4);

    screen::printf("\n\r");
    screen::printf("\n\r USB Device %u", (uint32_t)index);
    screen::printf("\n\r  Vendor ID:     0x%s", vid);
    screen::printf("\n\r  Product ID:    0x%s", pid);
    screen::printf("\n\r  USB Version:   %c.%c%c", bcd[1], bcd[2], bcd[3]);
    screen::printf("\n\r  Speed:         %s", usb::get_usb_speed_str(info.port_speed));
    screen::printf("\n\r  Slot:          %u", (uint32_t)info.slot_id);
    screen::printf("\n\r  Port:          %u", (uint32_t)info.port_index);
    screen::printf("\n\r  Class:         %s (0x%x)", usb::get_usb_class_name(info.device_class), (uint32_t)info.device_class);
    screen::printf("\n\r  Subclass:      0x%x", (uint32_t)info.device_subclass);
    screen::printf("\n\r  Protocol:      0x%x", (uint32_t)info.device_protocol);
    screen::printf("\n\r  Mass Storage:  %s", info.is_mass_storage ? "Yes" : "No");
    screen::printf("\n\r  Connected:     %s", info.connected ? "Yes" : "No");

    if (info.vendor_str[0] != '\0')
        screen::printf("\n\r  Vendor:        %s", info.vendor_str);
    if (info.product_str[0] != '\0')
        screen::printf("\n\r  Product:       %s", info.product_str);

    if (info.is_mass_storage)
    {
        uint8_t blk_count = usb::get_block_device_count();
        for (uint8_t b = 0; b < blk_count; b++)
        {
            usb_block_device bdev;
            if (usb::get_block_device_info(b, &bdev) != USB_OK)
                continue;

            screen::printf("\n\r  Block device:");
            screen::printf("\n\r    Block size:  %u bytes", bdev.block_size);
            screen::printf("\n\r    Last LBA:    %u", bdev.last_lba);

            uint64_t total_mb = bdev.total_bytes / (1024 * 1024);
            if (total_mb > 1024)
                screen::printf("\n\r    Capacity:    %u GB", (uint32_t)(total_mb / 1024));
            else
                screen::printf("\n\r    Capacity:    %u MB", (uint32_t)total_mb);

            screen::printf("\n\r    Ready:       %s", bdev.ready ? "Yes" : "No");
            break;
        }
    }
}

static void cmd_lsblk(int argc, const char** argv)
{
    (void)argc; (void)argv;
    screen::printf("\n\r");
    uart::printf("lsblk:\n");

    uint32_t count = block::count();
    if (count == 0)
    {
        screen::printf("\n\r No block devices found");
        uart::printf("lsblk: no block devices\n");
        return;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        blkdev* d = block::get(i);
        if (!d)
            continue;

        uint64_t total_mb = d->sector_count * d->sector_size / (1024 * 1024);

        screen::printf("\n\r %s%-9s %uB x %u",
            d->parent ? "  " : " ", d->name,
            d->sector_size, (uint32_t)d->sector_count);

        if (total_mb > 1024)
            screen::printf("  size=%u GB", (uint32_t)(total_mb / 1024));
        else
            screen::printf("  size=%u MB", (uint32_t)total_mb);

        if (d->parent)
            screen::printf("  offset=%u", (uint32_t)d->lba_offset);

        uart::printf("lsblk: %s %uB x %u offset %u\n", d->name,
                     d->sector_size, (uint32_t)d->sector_count,
                     (uint32_t)d->lba_offset);
    }
}

static void cmd_meminfo(int argc, const char** argv)
{
    screen::printf("\n\r");

    uint64_t total_ram = memory::total();
    uint64_t total_ram_mb = total_ram / (1024 * 1024);
    screen::printf("\n\r Physical RAM:      %u MB", (uint32_t)total_ram_mb);

    pmm::Stats pstats;
    pmm::get_stats(&pstats);
    // Mirrored to the serial log: the test harness compares the free-frame
    // count across sessions to catch a leaked per-process kernel stack.
    uart::printf("meminfo: frames_free=%u frames_used=%u\n",
                 (uint32_t)pstats.free_frames, (uint32_t)pstats.used_frames);
    screen::printf("\n\r");
    screen::printf("\n\r PMM frames (4 KB): %u total, %u free, %u used",
        (uint32_t)pstats.total_frames, (uint32_t)pstats.free_frames, (uint32_t)pstats.used_frames);
    screen::printf("\n\r PMM managed:       %u MB", (uint32_t)(pstats.max_phys / (1024 * 1024)));

    HeapStats stats;
    heap::get_stats(&stats);

    uint32_t total_kb = (uint32_t)(stats.total_size / 1024);
    uint32_t used_kb  = (uint32_t)(stats.used_size / 1024);
    uint32_t free_kb  = (uint32_t)(stats.free_size / 1024);

    screen::printf("\n\r");
    screen::printf("\n\r Heap total:        %u KB", total_kb);
    screen::printf("\n\r Heap used:         %u KB", used_kb);
    screen::printf("\n\r Heap free:         %u KB", free_kb);
    screen::printf("\n\r Largest free:      %u KB", (uint32_t)(stats.largest_free_block / 1024));
    screen::printf("\n\r");
    screen::printf("\n\r Blocks total:      %u", (uint32_t)stats.block_count);
    screen::printf("\n\r Blocks used:       %u", (uint32_t)stats.used_block_count);
    screen::printf("\n\r Blocks free:       %u", (uint32_t)stats.free_block_count);
}

static void cmd_time(int argc, const char** argv)
{
    rtc_time t;
    rtc::read(&t);

    screen::printf("\n\r");
    screen::printf("\n\r %02u:%02u:%02u  %02u.%02u.%u",
        (uint32_t)t.hours, (uint32_t)t.minutes, (uint32_t)t.seconds,
        (uint32_t)t.day, (uint32_t)t.month, (uint32_t)t.year);
}

static void cmd_uptime(int argc, const char** argv)
{
    uint64_t ms = pit::uptime_ms();

    uint32_t total_sec = (uint32_t)(ms / 1000);
    uint32_t hours = total_sec / 3600;
    uint32_t minutes = (total_sec % 3600) / 60;
    uint32_t seconds = total_sec % 60;
    uint32_t millis = (uint32_t)(ms % 1000);

    screen::printf("\n\r");
    screen::printf("\n\r Uptime: %u:%02u:%02u.%03u", hours, minutes, seconds, millis);
    screen::printf("\n\r Ticks:  %u", (uint32_t)pit::ticks());
    screen::printf("\n\r Freq:   %u Hz (real: %u Hz)", pit::frequency(), pit::real_frequency());
}

static void cmd_settime(int argc, const char** argv)
{
    // Usage: settime HH:MM:SS [DD.MM.YYYY]
    if (argc < 2)
    {
        screen::printf("\n\r Usage: settime HH:MM:SS [DD.MM.YYYY]");
        return;
    }

    // Parse time: HH:MM:SS
    const char* ts = argv[1];

    // Validate minimum length "H:M:S" = 5 chars
    uint32_t len = strlen(ts);
    if (len < 5)
    {
        screen::printf("\n\r Invalid time format");
        return;
    }

    // Read current RTC as base values
    rtc_time t;
    rtc::read(&t);

    // Find colons and parse
    char time_buf[16];
    if (len > 15) len = 15;
    memcpy(time_buf, ts, len);
    time_buf[len] = '\0';

    // Replace ':' with '\0' to split
    char* parts[3];
    uint32_t part_count = 0;
    parts[part_count++] = time_buf;
    for (uint32_t i = 0; i < len && part_count < 3; i++)
    {
        if (time_buf[i] == ':')
        {
            time_buf[i] = '\0';
            parts[part_count++] = &time_buf[i + 1];
        }
    }

    if (part_count < 3)
    {
        screen::printf("\n\r Invalid time format, use HH:MM:SS");
        return;
    }

    t.hours   = (uint8_t)parse_uint(parts[0]);
    t.minutes = (uint8_t)parse_uint(parts[1]);
    t.seconds = (uint8_t)parse_uint(parts[2]);

    // Validate
    if (t.hours > 23 || t.minutes > 59 || t.seconds > 59)
    {
        screen::printf("\n\r Invalid time values");
        return;
    }

    // Parse optional date: DD.MM.YYYY
    if (argc >= 3)
    {
        const char* ds = argv[2];
        uint32_t dlen = strlen(ds);

        char date_buf[16];
        if (dlen > 15) dlen = 15;
        memcpy(date_buf, ds, dlen);
        date_buf[dlen] = '\0';

        char* dparts[3];
        uint32_t dpart_count = 0;
        dparts[dpart_count++] = date_buf;
        for (uint32_t i = 0; i < dlen && dpart_count < 3; i++)
        {
            if (date_buf[i] == '.')
            {
                date_buf[i] = '\0';
                dparts[dpart_count++] = &date_buf[i + 1];
            }
        }

        if (dpart_count < 3)
        {
            screen::printf("\n\r Invalid date format, use DD.MM.YYYY");
            return;
        }

        t.day   = (uint8_t)parse_uint(dparts[0]);
        t.month = (uint8_t)parse_uint(dparts[1]);
        t.year  = (uint16_t)parse_uint(dparts[2]);

        if (t.day < 1 || t.day > 31 || t.month < 1 || t.month > 12)
        {
            screen::printf("\n\r Invalid date values");
            return;
        }
    }

    rtc::write(&t);

    screen::printf("\n\r Time set to %02u:%02u:%02u", (uint32_t)t.hours, (uint32_t)t.minutes, (uint32_t)t.seconds);

    if (argc >= 3) 
    {
        screen::printf("  %02u.%02u.%u", (uint32_t)t.day, (uint32_t)t.month, (uint32_t)t.year);
    }        
}

static void cmd_cd(int argc, const char** argv)
{
    if (argc < 2)
    {
        char buf[PATH_MAX];
        if (vfs::cwd_path(buf, sizeof(buf)) == 0)
            screen::printf("\n\r%s", buf);
        return;
    }

    vnode* v = nullptr;
    sint64_t rc = vfs::lookup(argv[1], vfs::cwd(), &v, true);
    if (rc != 0)
    {
        screen::printf("\n\rDirectory not found: %s", argv[1]);
        return;
    }
    vfs::set_cwd(v);        // takes the reference
}

static void cmd_exec(int argc, const char** argv)
{
    if (argc < 2)
    {
        screen::printf("\n\rUsage: exec <filename> [args...]   (Esc terminates the app)");
        return;
    }

    // argv[1..] becomes the program's argv, so argv[0] is its own name.
    int status = 0;
    screen::printf("\n\r");
    if (!process::run(argv[1], argc - 1, argv + 1, &status))
    {
        // PATH fallback: a bare name is looked up in /bin.
        char alt[NAME_MAX + 8];
        bool has_slash = false;
        for (const char* p = argv[1]; *p; p++)
            if (*p == '/')
                has_slash = true;

        if (!has_slash)
        {
            alt[0] = '/'; alt[1] = 'b'; alt[2] = 'i'; alt[3] = 'n'; alt[4] = '/';
            uint32_t n = 5;
            for (const char* p = argv[1]; *p && n < sizeof(alt) - 1; p++)
                alt[n++] = *p;
            alt[n] = '\0';

            if (process::run(alt, argc - 1, argv + 1, &status))
            {
                if (WIFSIGNALED(status))
                    screen::printf("%s: terminated by signal %u", argv[1],
                                   (uint32_t)WTERMSIG(status));
                else if (WEXITSTATUS(status) != 0)
                    screen::printf("%s: exited with status %u", argv[1],
                                   (uint32_t)WEXITSTATUS(status));
                return;
            }
        }

        screen::printf("Failed to load: %s", argv[1]);
        return;
    }

    if (WIFSIGNALED(status))
        screen::printf("%s: terminated by signal %u", argv[1], (uint32_t)WTERMSIG(status));
    else if (WEXITSTATUS(status) != 0)
        screen::printf("%s: exited with status %u", argv[1], (uint32_t)WEXITSTATUS(status));
}

namespace commands
{
    void init()
    {
        console::register_command("help",    cmd_help);
        console::register_command("cls",     cmd_cls);
        console::register_command("cpuid",   cmd_cpuid);
        console::register_command("lspci",   cmd_lspci);
        console::register_command("mount",   cmd_mount);
        console::register_command("umount",  cmd_umount);
        console::register_command("ls",      cmd_ls);
        console::register_command("cat",     cmd_cat);
        console::register_command("xxd",     cmd_xxd);
        console::register_command("write",   cmd_write);
        console::register_command("mkdir",   cmd_mkdir);
        console::register_command("rm",      cmd_rm);
        console::register_command("lsusb",   cmd_lsusb);
        console::register_command("usbports", cmd_usbports);
        console::register_command("dmesg",   cmd_dmesg);
        console::register_command("usbinfo", cmd_usbinfo);
        console::register_command("lsblk",   cmd_lsblk);
        console::register_command("sync",    cmd_sync);
        console::register_command("meminfo", cmd_meminfo);
        console::register_command("time",    cmd_time);
        console::register_command("uptime",  cmd_uptime);
        console::register_command("settime", cmd_settime);
        console::register_command("cd",      cmd_cd);
        console::register_command("pwd",     cmd_pwd);
        console::register_command("cp",      cmd_cp);
        console::register_command("mv",      cmd_mv);
        console::register_command("rmdir",   cmd_rmdir);
        console::register_command("exec",    cmd_exec);
    }
}