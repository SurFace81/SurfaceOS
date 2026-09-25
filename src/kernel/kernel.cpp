#include "../include/boot/boot.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/tss.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/cpu/features.h"
#include "../include/cpu/process.h"
#include "../include/cpu/syscall.h"
#include "../include/cpu/sfcall.h"
#include "../include/acpi/acpi.h"
#include "../include/dev/blkdev.h"
#include "../include/dev/part.h"
#include "../include/dev/bcache.h"
#include "../include/fs/vfs.h"
#include "../include/fs/file.h"
#include "../include/fs/fat32fs.h"
#include "../include/fs/devfs.h"
#include "../include/drivers/console.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/uart.h"
#include "../include/drivers/screen.h"
#include "../include/drivers/pit.h"
#include "../include/drivers/rtc.h"
#include "../include/mm/memory.h"
#include "../include/mm/heap.h"
#include "../include/mm/pmm.h"
#include "../include/drivers/usb/xhci.h"
#include "../include/stdlib/string.h"
#include "../sdk/include/abi/errno.h"
#include "../sdk/include/abi/dirent.h"

namespace
{
    // Does `disk` carry the signature the UEFI boot loader reported for the
    // boot volume?
    //   MBR (sigtype 1): the 4-byte disk signature at offset 440 of LBA 0.
    //   GPT (sigtype 2): the boot partition's UniquePartitionGUID (the UEFI
    //     HARDDRIVE_DP node carries the *partition* GUID, not the disk GUID):
    //     walk the GPT entry array and compare the GUID of the entry that
    //     starts at the reported LBA.
    bool disk_signature_matches(blkdev* disk, uint32_t sig_type,
                                const uint8_t* signature, uint64_t part_start)
    {
        uint8_t* sector = (uint8_t*)kmalloc(disk->sector_size);
        if (!sector)
            return false;

        bool ok = false;
        if (sig_type == 1)
        {
            if (block::read(disk, 0, 1, sector) == 0)
                ok = memory::memcmp(sector + 440, signature, 4) == 0;
        }
        else if (sig_type == 2 && disk->sector_count >= 2 &&
                 block::read(disk, 1, 1, sector) == 0)
        {
            // GPT header: entries_lba @72, num_entries @80, entry_size @84.
            if (memory::memcmp(sector, (const uint8_t*)"EFI PART", 8) == 0)
            {
                uint64_t entries_lba = *(const uint64_t*)(sector + 72);
                uint32_t num  = *(const uint32_t*)(sector + 80);
                uint32_t esz  = *(const uint32_t*)(sector + 84);

                if (esz >= 128 && num > 0 && num <= 128 &&
                    entries_lba < disk->sector_count)
                {
                    for (uint32_t i = 0; i < num && !ok; i++)
                    {
                        uint64_t lba = entries_lba + (uint64_t)i * esz / disk->sector_size;
                        uint32_t in_off = (uint32_t)((uint64_t)i * esz % disk->sector_size);

                        if (in_off + 128 > disk->sector_size)
                            break;                  // entry straddles sectors: skip
                        if (block::read(disk, lba, 1, sector) != 0)
                            break;

                        const uint8_t* e = sector + in_off;
                        // Empty slot: zero type GUID.
                        static const uint8_t zero16[16] = {0};
                        if (memory::memcmp(e, zero16, 16) == 0)
                            continue;

                        uint64_t first_lba = *(const uint64_t*)(e + 32);
                        const uint8_t* uniq = e + 16;
                        if (first_lba == part_start &&
                            memory::memcmp(uniq, signature, 16) == 0)
                            ok = true;
                    }
                }
            }
        }

        kfree(sector);
        return ok;
    }

    // Mount the volume UEFI booted from as the root (stage 3.2).
    //
    // 1. The bootloader hands over the boot partition's LBA and the disk
    //    signature (MBR) or partition GUID (GPT) from the device path of the
    //    loaded image. A blkdev matches when it starts at that LBA on a disk
    //    with that signature.
    // 2. No match (unknown layout, several sticks, ...): take the first
    //    volume with a /sfos/KERNEL.BIN - that is our kernel, so that
    //    volume is where we booted from in all but the strangest setups.
    // 3. Nothing at all: say so explicitly and run the console rootless;
    //    every fs command then fails cleanly instead of hanging.
    // Try to mount `d` as the VFS root; on success the system cwd starts at
    // the new root.
    bool try_mount_root(blkdev* d)
    {
        if (vfs::mount_at(nullptr, d->name, &fat32fs::fs, d) != 0)
            return false;

        mount* m = vfs::root_mount();
        vfs::ref(m->root);
        vfs::set_cwd(m->root);
        return true;
    }

    void unmount_root()
    {
        vfs::set_cwd(nullptr);
        mount* m = vfs::root_mount();
        if (m)
            vfs::umount(m, true);   // shutdown: tear down regardless of fds
    }

    // What the loader did to VT-d (it has no way to print after
    // ExitBootServices). Also on screen: the laptop that needs this has no
    // serial port.
    void report_dmar(const BOOT_HEADER* hdr)
    {
        uart::printf("boot: acpi rsdp=%llx\n", hdr->AcpiRsdpAddress);

        if (!(hdr->DmarFlags & BOOT_DMAR_PRESENT))
        {
            uart::printf("boot: no DMAR table, no VT-d to take over\n");
            return;
        }

        uart::printf("boot: DMAR %u unit(s), remapping was %s, turned off on %u%s\n",
                     hdr->DmarUnits,
                     (hdr->DmarFlags & BOOT_DMAR_WAS_ENABLED) ? "on" : "off",
                     hdr->DmarDisabled,
                     (hdr->DmarFlags & BOOT_DMAR_TIMEOUT) ? " (TIMEOUT)" : "");
        if (hdr->DmarFlags & BOOT_DMAR_WAS_ENABLED)
            screen::printf("VT-d: %u unit(s), remapping turned off on %u%s\n\r",
                           hdr->DmarUnits, hdr->DmarDisabled,
                           (hdr->DmarFlags & BOOT_DMAR_TIMEOUT) ? ", TIMEOUT" : "");
    }

    void automount_root(const BOOT_HEADER* hdr)
    {
        // Pass 1: exact boot-volume match.
        for (uint32_t i = 0; block::get(i); i++)
        {
            blkdev* d = block::get(i);

            bool plausible;
            if (d->parent)
                plausible = d->lba_offset == hdr->BootPartitionStart;
            else
                plausible = hdr->BootPartitionStart == 0;   // superfloppy

            if (!plausible)
                continue;
            if (hdr->BootDevicePathValid && hdr->BootPartitionSignatureType != 0)
            {
                blkdev* disk = d->parent ? d->parent : d;
                if (!disk_signature_matches(disk, hdr->BootPartitionSignatureType,
                                            hdr->BootPartitionSignature,
                                            hdr->BootPartitionStart))
                    continue;
            }

            if (try_mount_root(d))
            {
                uart::printf("boot: root mounted on %s (boot volume)\n", d->name);
                screen::printf("Root: %s\n\r", d->name);
                return;
            }
        }

        if (hdr->BootDevicePathValid)
            uart::printf("boot: no blkdev matches the UEFI boot volume "
                         "(start %u sigtype %u), falling back to KERNEL.BIN scan\n",
                         (uint32_t)hdr->BootPartitionStart,
                         hdr->BootPartitionSignatureType);

        // Pass 2: first volume that contains our kernel image.
        for (uint32_t i = 0; block::get(i); i++)
        {
            blkdev* d = block::get(i);
            if (!try_mount_root(d))
                continue;

            vnode* v = nullptr;
            if (vfs::lookup("/sfos/KERNEL.BIN", nullptr, &v, false) == 0)
            {
                vfs::unref(v);
                uart::printf("boot: root mounted on %s (KERNEL.BIN found)\n", d->name);
                screen::printf("Root: %s\n\r", d->name);
                return;
            }
            unmount_root();
        }

        uart::printf("boot: no mountable FAT32 volume found - running without root\n");
        screen::printf("No root volume found; fs commands unavailable.\n\r");

        // There is no serial port on most laptops, so put enough on the
        // screen to tell the three failure modes apart: no controller at
        // all, a controller with no devices, and devices with no volume.
        uint32_t blkdevs = 0;
        while (block::get(blkdevs))
            blkdevs++;

        uint8_t bus = 0, dev = 0, fn = 0;
        usb::get_controller_location(&bus, &dev, &fn);
        // screen::printf("  xhci: %u controller(s), using %u:%u.%u; "
        //                "%u usb device(s), %u disk(s), %u volume(s)\n\r",
        //                (uint32_t)usb::get_controller_count(),
        //                (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
        //                (uint32_t)usb::get_device_count(),
        //                (uint32_t)usb::get_block_device_count(),
        //                blkdevs);
    }

    // Look up the top-level directory /<name>, creating it on the root
    // volume when it is missing (an image made by hand, or an older mkimg).
    // *created is set when the directory had to be made.
    sint64_t ensure_root_dir(const char* name, bool* created)
    {
        char path[16];
        path[0] = '/';
        uint32_t n = 1;
        for (const char* p = name; *p && n < sizeof(path) - 1; p++)
            path[n++] = *p;
        path[n] = '\0';

        vnode* dir = nullptr;
        sint64_t rc = vfs::lookup(path, nullptr, &dir, true);
        if (rc == -ENOENT)
        {
            vnode* root = nullptr;
            rc = vfs::lookup("/", nullptr, &root, true);
            if (rc == 0 && root->ops->mkdir)
            {
                rc = root->ops->mkdir(root, name, 0755);
                if (rc == 0)
                {
                    uart::printf("boot: created %s\n", path);
                    *created = true;
                }
            }
            if (root)
                vfs::unref(root);
        }

        if (dir)
            vfs::unref(dir);
        return rc;
    }

    // Delete everything inside `dir`, subdirectories included. Deeper than
    // TMP_MAX_DEPTH is left alone: each level costs a dirent_out on the
    // kernel stack. Returns the number of entries removed.
    const uint32_t TMP_MAX_DEPTH = 16;

    uint32_t clear_dir(vnode* dir, uint32_t depth)
    {
        uint32_t removed = 0;
        uint64_t cookie = 0;
        for (;;)
        {
            // FAT marks a deleted entry in place, so the cookie stays valid
            // across the unlinks below.
            dirent_out d;
            bool eof = false;
            if (dir->ops->readdir(dir, &cookie, &d, &eof) != 0 || eof)
                break;
            if (strcmp(d.name, ".") == 0 || strcmp(d.name, "..") == 0)
                continue;

            sint64_t rc;
            if (d.type == DT_DIR)
            {
                if (depth + 1 >= TMP_MAX_DEPTH)
                {
                    uart::printf("boot: /tmp too deep, %s left in place\n", d.name);
                    continue;
                }
                vnode* sub = nullptr;
                rc = vfs::lookup(d.name, dir, &sub, true);
                if (rc == 0)
                {
                    removed += clear_dir(sub, depth + 1);
                    vfs::unref(sub);
                    rc = dir->ops->rmdir(dir, d.name);
                }
            }
            else
                rc = dir->ops->unlink(dir, d.name);

            if (rc == 0)
                removed++;
            else
                uart::printf("boot: cannot remove %s from /tmp (%d)\n",
                             d.name, (int)rc);
        }
        return removed;
    }

    // Mount points left in /mount by a power cut without umount: remove the
    // empty directories. A non-empty one is someone's data and stays.
    uint32_t clear_stale_mount_points(vnode* dir)
    {
        uint32_t removed = 0;
        uint64_t cookie = 0;
        for (;;)
        {
            dirent_out d;
            bool eof = false;
            if (dir->ops->readdir(dir, &cookie, &d, &eof) != 0 || eof)
                break;
            if (d.type != DT_DIR || strcmp(d.name, ".") == 0 ||
                strcmp(d.name, "..") == 0)
                continue;
            if (dir->ops->rmdir(dir, d.name) == 0)
                removed++;
        }
        return removed;
    }
}

extern "C" void kmain(uint64_t boot_header_phys)
{
    // kentry.asm has already zeroed .bss, switched to the kernel stack and
    // moved us to the higher half. Its boot tables direct-map the first
    // 1 GiB, which is where the loader put the boot header.
    BOOT_HEADER* BootHeader = (BOOT_HEADER*)phys_to_virt(boot_header_phys);

    // Init order is load-bearing:
    //   features before paging  - PAGE_NX is a reserved bit until EFER.NXE
    //                             is set, and PAGE_CACHE_WC means nothing
    //                             until the PAT is programmed.
    //   paging  before pmm      - the PMM refuses to manage memory the
    //                             direct map does not cover.
    //   uart    before pmm      - so the memory report is actually visible.
    //   pmm     before screen   - the back buffer is a PMM allocation now.
    cpu::init_features();

    gdt::init();
    tss::init();
    // The bootloader AllocatePages()es 5 MB at PAGE_TABLES_PHYS and
    // linker.ld asserts that the kernel image stops short of it.
    paging::init(PAGE_TABLES_PHYS, BootHeader);

    idt::init();
    irq::init();

    pci::init();
    uart::init();
    cpu::log_features();

    pmm::init(BootHeader);
    memory::init(BootHeader->TotalMemorySize);

    // Tables only, no AML: just enough for reboot/shutdown.
    acpi::init(BootHeader);

    // From here on every stage announces itself on the serial line. On real
    // hardware a hang before the timer IRQ starts flushing the back buffer
    // leaves a black screen and nothing else to go on.
    uart::printf("boot: heap ready\n");

    screen::init(BootHeader);
    uart::printf("boot: screen %ux%u fb=%llx vram=%llx\n",
                 BootHeader->ScreenWidth, BootHeader->ScreenHeight,
                 (uint64_t)BootHeader->FrameBufferAddress,
                 (uint64_t)screen::vram_base());
    report_dmar(BootHeader);

    keyboard::init();
    uart::printf("boot: keyboard ready\n");

    // Timers come up before USB: the xHCI driver measures its timeouts in
    // real milliseconds off the PIT, so it needs a calibrated tick first.
    pit::init();
    rtc::init();
    irq::install_handler(IRQ0_TIMER, pit::handler);
    irq::install_handler(IRQ1_KEYBOARD, keyboard::handler);
    pit::calibrate();
    uart::printf("boot: pit calibrated at %u Hz\n", pit::real_frequency());

    usb::init();
    uart::printf("boot: usb ready\n");

    // Block layer: whole disks from USB MSD, their partitions, then the
    // sector cache (sized from the devices it found).
    block::enumerate_usb();
    part::enumerate();
    bcache::init();
    vfs::init();
    filesys::init();

    automount_root(BootHeader);

    // The top-level directories come from the disk image (tools/mkimg.py
    // creates them); the ones that are missing are created on the root
    // volume. /tmp starts empty on every boot, and /mount loses the empty
    // mount points a power cut left. All of it is flushed right away, so a
    // power cut does not undo it.
    if (vfs::root_mount())
    {
        static const char* const dirs[] = { "files", "tmp", "mount" };
        bool changed = false;
        for (const char* d : dirs)
        {
            sint64_t rc = ensure_root_dir(d, &changed);
            if (rc != 0)
                uart::printf("boot: /%s unavailable (%d)\n", d, (int)rc);
        }

        vnode* tmp = nullptr;
        if (vfs::lookup("/tmp", nullptr, &tmp, true) == 0)
        {
            uint32_t removed = clear_dir(tmp, 0);
            vfs::unref(tmp);
            if (removed)
            {
                uart::printf("boot: /tmp cleared, %u entries removed\n", removed);
                changed = true;
            }
        }

        vnode* mnt = nullptr;
        if (vfs::lookup("/mount", nullptr, &mnt, true) == 0)
        {
            uint32_t removed = clear_stale_mount_points(mnt);
            vfs::unref(mnt);
            if (removed)
            {
                uart::printf("boot: %u stale mount point(s) removed\n", removed);
                changed = true;
            }
        }

        if (changed)
            vfs::sync_all();
    }

    // devfs lives outside the tree: processes get the tty through it.
    if (devfs::init() == 0)
        uart::printf("boot: devfs mounted\n");

    syscall::init();
    sfcall::init();
    process::init();

    console::init();
    process::start_console(console::main);
    uart::printf("boot: console ready\n");

    // From here on the boot task is the idle task: the console and the
    // programs it starts run as scheduled tasks.
    process::idle();
}
