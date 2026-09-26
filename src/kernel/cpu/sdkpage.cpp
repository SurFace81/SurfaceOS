// The SDK pages of a SurfaceOS program. See sdkpage.h.

#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/tty.h"
#include "../../include/drivers/uart.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/abi/sdkimage.h"
#include "../../sdk/include/sfos.h"

extern "C" const uint8_t sdk_runtime_start[];
extern "C" const uint8_t sdk_runtime_end[];

namespace
{
    const uint64_t CODE_PAGES_MAX = SDK_CODE_MAX / PAGE_SIZE_4K;

    // The runtime's code, copied once; every process maps these frames.
    uint64_t code_frames[CODE_PAGES_MAX];
    uint64_t code_pages  = 0;
    uint64_t start       = 0;       // SdkHeader.Start
    uint64_t state_pages = 0;

    // Map one zeroed page at vaddr; its kernel (direct-map) address, or
    // nullptr.
    uint8_t* map_page(uint64_t vaddr, uint64_t flags)
    {
        uint64_t frame = pmm::alloc_frame();
        if (!frame)
            return nullptr;
        uint8_t* k = (uint8_t*)phys_to_virt(frame);
        memory::memset(k, 0x00, PAGE_SIZE_4K);
        if (!paging::map_user_page(vaddr, frame, flags))
        {
            pmm::free_frame(frame);
            return nullptr;
        }
        return k;
    }
}

namespace sdkpage
{
    void init()
    {
        uint64_t len = (uint64_t)(sdk_runtime_end - sdk_runtime_start);
        const SdkHeader* h = (const SdkHeader*)sdk_runtime_start;
        if (len < sizeof(SdkHeader) || len > SDK_CODE_MAX ||
            h->Magic != SDK_HEADER_MAGIC || h->StateStart != SDK_STATE_ADDRESS ||
            h->StateEnd < h->StateStart || h->StateEnd - h->StateStart > SDK_CODE_MAX)
        {
            uart::printf("sdkpage: the SDK runtime is broken, programs cannot start\n");
            return;
        }

        uint64_t pages = (len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        for (uint64_t i = 0; i < pages; i++)
        {
            uint64_t frame = pmm::alloc_frame();
            if (!frame)
                return;             // install() then fails every program
            uint8_t* page = (uint8_t*)phys_to_virt(frame);
            uint64_t off = i * PAGE_SIZE_4K;
            uint64_t n = len - off < PAGE_SIZE_4K ? len - off : PAGE_SIZE_4K;
            memory::memset(page, 0x00, PAGE_SIZE_4K);
            memory::memcpy(page, sdk_runtime_start + off, n);
            code_frames[i] = frame;
        }
        code_pages  = pages;
        start       = h->Start;
        state_pages = (h->StateEnd - h->StateStart + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        uart::printf("sdkpage: SDK runtime %u bytes, %u state pages\n",
                     (uint32_t)len, (uint32_t)state_pages);
    }

    bool install(const char* name, uint64_t* out_start)
    {
        if (!code_pages)
            return false;

        // R + X, and not the process's own frames.
        for (uint64_t i = 0; i < code_pages; i++)
            if (!paging::map_user_page(SDK_CODE_ADDRESS + i * PAGE_SIZE_4K, code_frames[i],
                                       PAGE_SHARED))
                return false;

        SdkStartInfo* info = (SdkStartInfo*)map_page(SDK_INFO_ADDRESS, PAGE_NX);    // R
        if (!info)
            return false;
        uint32_t n = 0;
        for (; name[n] && n < sizeof(info->Name) - 1; n++)
            info->Name[n] = name[n];
        info->Name[n] = '\0';

        for (uint64_t i = 0; i < state_pages; i++)
            if (!map_page(SDK_STATE_ADDRESS + i * PAGE_SIZE_4K, PAGE_WRITE | PAGE_NX))
                return false;

        *out_start = start;
        return true;
    }

    void console_print(user_regs* regs, iret_frame*)
    {
        const uint64_t CHUNK = 1024;
        char* buf = (char*)kmalloc(CHUNK);
        if (!buf)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        // Any length, a chunk at a time: -2 means the chunk is full and the
        // text goes on.
        uint64_t text = regs->rdi;
        for (;;)
        {
            sint64_t len = uaccess::strncpy_from_user(buf, text, CHUNK);
            if (len == -1)
            {
                regs->rax = SF_INVALID_PARAMETER;
                break;
            }
            if (len >= 0)
            {
                tty::write(buf, (uint64_t)len);
                regs->rax = SF_SUCCESS;
                break;
            }
            tty::write(buf, CHUNK - 1);
            text += CHUNK - 1;
        }
        kfree(buf);
    }

    void console_readline(user_regs* regs, iret_frame*)
    {
        uint64_t buffer = regs->rdi;
        uint64_t size   = regs->rsi;
        uint64_t length = regs->rdx;
        if (!size)
        {
            regs->rax = SF_INVALID_PARAMETER;
            return;
        }

        // The tty's line is at most 1 KiB, so one read takes all of it.
        const uint64_t LINE = 1024;
        char* line = (char*)kmalloc(LINE);
        if (!line)
        {
            regs->rax = SF_OUT_OF_RESOURCES;
            return;
        }

        sint64_t n;
        while ((n = tty::read(line, LINE)) == -EAGAIN)
        {
            if (!tty::wait_readable())
            {
                // Ctrl+C (or ^Z) ended the wait.
                kfree(line);
                regs->rax = SF_ABORTED;
                return;
            }
        }
        if (n == 0)
        {
            kfree(line);
            regs->rax = SF_END_OF_FILE;
            return;
        }

        uint64_t len = (uint64_t)n;
        if (line[len - 1] == '\n')
            len--;
        if (len > size - 1)
            len = size - 1;             // a longer line is cut
        line[len] = '\0';

        bool ok = uaccess::copy_to_user(buffer, line, len + 1) &&
                  (!length || uaccess::copy_to_user(length, &len, sizeof(len)));
        kfree(line);
        regs->rax = ok ? SF_SUCCESS : SF_INVALID_PARAMETER;
    }
}
