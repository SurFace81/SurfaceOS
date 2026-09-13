// Process execution: runs flat-binary apps in ring 3 inside a dedicated
// address space. Currently synchronous (one app at a time, blocking the
// shell); the preemptive scheduler builds on top of this module later.

#include "../../include/cpu/process.h"
#include "../../include/cpu/tss.h"
#include "../../include/cpu/elf.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/fs/fat32.h"
#include "../../include/drivers/keyboard.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/uart.h"
#include "../../include/cpu/irq.h"

extern "C" void process_enter_user(uint64_t entry, uint64_t user_stack, uint64_t arg);
extern "C" void process_return_to_kernel(void);

namespace process
{
    // Kernel stack used while the app runs (interrupts/syscalls land here
    // via TSS RSP0)
    static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

    // Program break state (brk). Only one process runs at a time, so
    // static state is sufficient.
    static uint64_t heap_start = 0;   // set at load time (image_end)
    static uint64_t heap_brk   = 0;   // current program break
    static uint64_t heap_max   = 0;   // upper bound (USER_INFO_VADDR)

    // Ring buffer for keyboard events while the app is running
    static const uint32_t KEY_BUF_SIZE = 64;
    static keyboard_event_t key_buf[KEY_BUF_SIZE];
    static volatile uint32_t key_head = 0;
    static volatile uint32_t key_tail = 0;

    static void app_key_handler(keyboard_event_t e)
    {
        uint32_t next = (key_head + 1) % KEY_BUF_SIZE;
        if (next == key_tail)
            return; // buffer full, drop event

        key_buf[key_head] = e;
        key_head = next;
    }

    bool has_key()
    {
        return key_head != key_tail;
    }

    keyboard_event_t pop_key()
    {
        keyboard_event_t e = key_buf[key_tail];
        key_tail = (key_tail + 1) % KEY_BUF_SIZE;
        return e;
    }

    // Map [vaddr, vaddr+size) with zeroed user-writable 4 KiB pages.
    static bool map_user_region(uint64_t vaddr, uint64_t size)
    {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE_4K)
        {
            uint64_t frame = pmm::alloc_frame();
            if (!frame)
                return false;

            memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
            if (!paging::map_page(vaddr + off, frame, PAGE_WRITE | PAGE_USER))
                return false;
        }
        return true;
    }

    bool run(const char* path)
    {
        // Load the app image into a kernel buffer first
        uint8_t* image = (uint8_t*)kmalloc(USER_IMAGE_MAX);
        if (!image)
            return false;

        uint32_t bytes_read = fat32::read_file(path, image, USER_IMAGE_MAX);
        if (bytes_read == (uint32_t)-1 || bytes_read == 0)
        {
            kfree(image);
            return false;
        }

        // Fresh address space
        uint64_t as = paging::create_address_space();
        if (!as)
        {
            kfree(image);
            return false;
        }
        paging::switch_address_space(as);

        // Map the info page and the stack region (always present).
        // Code/data/heap come from the ELF loader or the flat-binary path.
        if (!map_user_region(USER_INFO_VADDR, PAGE_SIZE_4K) ||
            !map_user_region(USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE))
        {
            paging::switch_address_space(paging::kernel_pml4());
            paging::destroy_address_space(as);
            kfree(image);
            return false;
        }

        uint64_t entry;
        uint64_t image_end;

        if (elf::is_elf(image, bytes_read))
        {
            // ELF64 executable: load PT_LOAD segments with real permissions
            elf::LoadResult lr = elf::load(image, bytes_read);
            if (!lr.valid)
            {
                paging::switch_address_space(paging::kernel_pml4());
                paging::destroy_address_space(as);
                kfree(image);
                return false;
            }
            entry = lr.entry;
            image_end = lr.image_end;
        }
        else
        {
            // Legacy flat binary: map a RWX window and copy it in
            uint64_t image_size = (bytes_read + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
            if (!map_user_region(USER_IMAGE_VADDR, image_size))
            {
                paging::switch_address_space(paging::kernel_pml4());
                paging::destroy_address_space(as);
                kfree(image);
                return false;
            }

            uint64_t image_pages = (bytes_read + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
            for (uint64_t p = 0; p < image_pages; p++)
            {
                uint64_t vaddr = USER_IMAGE_VADDR + p * PAGE_SIZE_4K;
                uint64_t phys = paging::virtual_to_phys(vaddr);
                uint64_t chunk = PAGE_SIZE_4K;
                uint64_t left = bytes_read - p * PAGE_SIZE_4K;
                if (left < chunk)
                    chunk = left;
                memory::memcpy((uint8_t*)phys, image + p * PAGE_SIZE_4K, chunk);
            }

            entry = USER_IMAGE_VADDR;
            image_end = USER_IMAGE_VADDR + image_size;
        }

        // The heap (image_end .. USER_INFO_VADDR) is NOT pre-mapped; the
        // app grows it on demand via the SYS_BRK syscall.

        // Program info visible to the app at USER_INFO_VADDR
        program_info* info = (program_info*)paging::virtual_to_phys(USER_INFO_VADDR);
        info->heap_start = image_end;
        info->heap_size  = USER_INFO_VADDR - image_end;

        // Initialize brk state for this process
        heap_start = image_end;
        heap_brk   = image_end;
        heap_max   = USER_INFO_VADDR;

        // Kernel stack for interrupts and syscalls from this app
        uint64_t kstack_top = (uint64_t)kernel_stack + KERNEL_STACK_SIZE;
        tss::set_kernel_stack(kstack_top);

        // Redirect keyboard events into the app queue
        key_head = 0;
        key_tail = 0;
        keyboard_callback_t prev_callback = keyboard::get_callback();
        keyboard::set_keyboard_callback(app_key_handler);

        // Draw title bar and shrink the app viewport (legacy app UX)
        screen::hide_cursor();
        screen::clear();

        const char* name = path;
        for (const char* p = path; *p; p++)
        {
            if (*p == '\\' || *p == '/')
                name = p + 1;
        }
        screen::draw_title_bar(name);

        uint32_t bar_h = 20; // sym_h(16) + 4px padding
        screen::push_viewport(screen::vp_x(), screen::vp_y() + bar_h,
                              screen::vp_w(), screen::vp_h() - bar_h);

        // We are typically invoked from inside the keyboard IRQ handler
        // (shell on_key -> exec). The PIC EOI for that key is normally sent
        // only after the IRQ handler returns, which won't happen until the
        // app exits. Acknowledge IRQ1 now so the PIC keeps delivering keys.
        irq::pic_send_eoi(IRQ1_KEYBOARD);

        // Enter ring 3; returns here when the app exits or faults
        process_enter_user(entry, USER_STACK_TOP, USER_INFO_VADDR);

        screen::pop_viewport();
        screen::clear();
        screen::show_cursor();

        keyboard::set_keyboard_callback(prev_callback);
        paging::switch_address_space(paging::kernel_pml4());
        paging::destroy_address_space(as);
        kfree(image);

        return true;
    }

    uint64_t brk(uint64_t new_brk)
    {
        // Query
        if (new_brk == 0)
            return heap_brk;

        // Reject out-of-range requests
        if (new_brk < heap_start || new_brk > heap_max)
            return heap_brk;

        // Page-align the new break upward
        uint64_t new_brk_page = (new_brk + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        uint64_t old_brk_page = (heap_brk + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);

        // Grow: map new pages
        if (new_brk_page > old_brk_page)
        {
            for (uint64_t page = old_brk_page; page < new_brk_page; page += PAGE_SIZE_4K)
            {
                uint64_t frame = pmm::alloc_frame();
                if (!frame)
                    return heap_brk;   // partial growth: return old break

                memory::memset((uint8_t*)frame, 0x00, PAGE_SIZE_4K);
                if (!paging::map_page(page, frame, PAGE_WRITE | PAGE_USER))
                {
                    pmm::free_frame(frame);
                    return heap_brk;
                }
            }
        }
        // Shrink: unmap pages beyond the new break
        else if (new_brk_page < old_brk_page)
        {
            for (uint64_t page = new_brk_page; page < old_brk_page; page += PAGE_SIZE_4K)
            {
                uint64_t phys = paging::virtual_to_phys(page);
                paging::unmap_page(page);
                if (phys)
                    pmm::free_frame(phys);
            }
        }

        heap_brk = new_brk;
        return heap_brk;
    }

    void exit_current()
    {
        // Restore the kernel stack saved by process_enter_user and
        // continue inside run(). Never returns.
        process_return_to_kernel();
        while (1) asm volatile("hlt");
    }
} // namespace process
