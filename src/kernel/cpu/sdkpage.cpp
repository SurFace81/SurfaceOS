// The SDK pages of a SurfaceOS program. See sdkpage.h.

#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/uart.h"
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
    uint64_t thread      = 0;       // SdkHeader.ThreadStart
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
        thread      = h->ThreadStart;
        state_pages = (h->StateEnd - h->StateStart + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        uart::printf("sdkpage: SDK runtime %u bytes, %u state pages\n",
                     (uint32_t)len, (uint32_t)state_pages);
    }

    uint64_t thread_start()
    {
        return thread;
    }

    bool install(const char* name, const char* args, uint32_t args_size, uint32_t argc,
                 uint64_t* out_start)
    {
        if (!code_pages)
            return false;

        // R + X, and not the process's own frames.
        for (uint64_t i = 0; i < code_pages; i++)
            if (!paging::map_user_page(SDK_CODE_ADDRESS + i * PAGE_SIZE_4K, code_frames[i],
                                       PAGE_SHARED))
                return false;

        // The start info: SdkStartInfo, the Args pointers, the strings.
        // Built here, then copied onto its read-only pages.
        uint64_t ptrs  = sizeof(SdkStartInfo);
        uint64_t strs  = ptrs + (uint64_t)argc * sizeof(uint64_t);
        uint64_t total = strs + args_size;
        if (total > SDK_CODE_MAX)
            return false;
        uint8_t* blob = (uint8_t*)kmalloc(total);
        if (!blob)
            return false;
        memory::memset(blob, 0x00, total);

        SdkStartInfo* info = (SdkStartInfo*)blob;
        uint32_t n = 0;
        for (; name[n] && n < sizeof(info->Name) - 1; n++)
            info->Name[n] = name[n];
        info->Name[n] = '\0';
        info->ArgCount = argc;
        info->Args     = (const char* const*)(SDK_INFO_ADDRESS + ptrs);

        memory::memcpy(blob + strs, (const uint8_t*)args, args_size);
        uint64_t* arg_ptrs = (uint64_t*)(blob + ptrs);
        uint64_t off = 0;
        for (uint32_t i = 0; i < argc; i++)
        {
            arg_ptrs[i] = SDK_INFO_ADDRESS + strs + off;
            while (off < args_size && args[off])
                off++;
            off++;                                  // the NUL
        }

        bool ok = true;
        for (uint64_t page = 0; ok && page * PAGE_SIZE_4K < total; page++)
        {
            uint8_t* k = map_page(SDK_INFO_ADDRESS + page * PAGE_SIZE_4K, PAGE_NX);   // R
            uint64_t start = page * PAGE_SIZE_4K;
            uint64_t len = total - start < PAGE_SIZE_4K ? total - start : PAGE_SIZE_4K;
            if (k)
                memory::memcpy(k, blob + start, len);
            ok = k != nullptr;
        }
        kfree(blob);
        if (!ok)
            return false;

        for (uint64_t i = 0; i < state_pages; i++)
            if (!map_page(SDK_STATE_ADDRESS + i * PAGE_SIZE_4K, PAGE_WRITE | PAGE_NX))
                return false;

        *out_start = start;
        return true;
    }
}
