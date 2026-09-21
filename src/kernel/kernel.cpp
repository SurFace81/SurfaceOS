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
#include "../include/dev/blkdev.h"
#include "../include/dev/part.h"
#include "../include/dev/bcache.h"
#include "../include/fs/vfs.h"
#include "../include/fs/file.h"
#include "../include/fs/fat32fs.h"
#include "../include/drivers/console.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/uart.h"
#include "../include/drivers/screen.h"
#include "../include/drivers/pit.h"
#include "../include/drivers/rtc.h"
#include "../include/drivers/fs/fat32.h"
#include "../include/mm/memory.h"
#include "../include/mm/heap.h"
#include "../include/mm/pmm.h"
#include "../include/drivers/usb/xhci.h"
#include "../include/stdlib/string.h"

// Base of the static page tables. The bootloader AllocatePages()es 5 MB here
// and linker.ld asserts that the kernel image stops short of it.
#define PAGE_TABLE_BASE 0x300000

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
    //    volume with a KERNEL.BIN in its root - that is our kernel, so that
    //    volume is where we booted from in all but the strangest setups.
    // 3. Nothing at all: say so explicitly and run the console rootless;
    //    every fs command then fails cleanly instead of hanging.
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

            if (fat32::mount(d))
            {
                uart::printf("boot: root mounted on %s (boot volume)\n", d->name);
                screen::printf("Root: %s\n\r", d->name);
                return;
            }
            fat32::umount();
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
            if (!fat32::mount(d))
                continue;

            fat32_dir_entry entry;
            if (fat32::resolve_path_pub("/KERNEL.BIN", &entry))
            {
                uart::printf("boot: root mounted on %s (KERNEL.BIN found)\n", d->name);
                screen::printf("Root: %s\n\r", d->name);
                return;
            }
            fat32::umount();
        }

        uart::printf("boot: no mountable FAT32 volume found - running without root\n");
        screen::printf("No root volume found; fs commands unavailable.\n\r");
    }
}

extern "C" void kmain(BOOT_HEADER* BootHeader)
{
    // kentry.asm has already zeroed .bss and switched to the kernel stack.
    //
    // Init order is load-bearing:
    //   features before paging  - PAGE_NX is a reserved bit until EFER.NXE
    //                             is set, and PAGE_CACHE_WC means nothing
    //                             until the PAT is programmed.
    //   paging  before pmm      - the PMM refuses to manage memory the
    //                             identity map does not cover.
    //   uart    before pmm      - so the memory report is actually visible.
    //   pmm     before screen   - the back buffer is a PMM allocation now.
    cpu::init_features();

    gdt::init();
    tss::init();
    paging::init((uint64_t*)PAGE_TABLE_BASE, BootHeader);

    idt::init();
    irq::init();

    pci::init();
    uart::init();
    cpu::log_features();

    pmm::init(BootHeader);
    memory::init(BootHeader->TotalMemorySize);

    // From here on every stage announces itself on the serial line. On real
    // hardware a hang before the timer IRQ starts flushing the back buffer
    // leaves a black screen and nothing else to go on.
    uart::printf("boot: heap ready\n");

    screen::init(BootHeader);
    uart::printf("boot: screen %ux%u fb=%llx vram=%llx\n",
                 BootHeader->ScreenWidth, BootHeader->ScreenHeight,
                 (uint64_t)BootHeader->FrameBufferAddress,
                 (uint64_t)screen::vram_base());

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

    syscall::init();
    process::init();

    console::init();
    uart::printf("boot: console ready\n");

    while (1)
    {
        // Execute any command queued by the keyboard handler (e.g. `exec`).
        // Runs in process context, not in the keyboard IRQ.
        console::poll();
        asm volatile("hlt");
    }
}
