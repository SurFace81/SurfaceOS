#include "../include/boot/boot.h"
#include "../include/cpu/gdt.h"
#include "../include/cpu/tss.h"
#include "../include/cpu/idt.h"
#include "../include/cpu/irq.h"
#include "../include/cpu/paging.h"
#include "../include/cpu/pci.h"
#include "../include/cpu/features.h"
#include "../include/cpu/process.h"
#include "../include/drivers/console.h"
#include "../include/drivers/keyboard.h"
#include "../include/drivers/uart.h"
#include "../include/drivers/pit.h"
#include "../include/drivers/rtc.h"
#include "../include/mm/memory.h"
#include "../include/mm/heap.h"
#include "../include/mm/pmm.h"
#include "../include/drivers/usb/xhci.h"

// Base of the static page tables. The bootloader AllocatePages()es 5 MB here
// and linker.ld asserts that the kernel image stops short of it.
#define PAGE_TABLE_BASE 0x300000

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
