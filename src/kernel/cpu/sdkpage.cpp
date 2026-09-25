// The SDK pages of a SurfaceOS program. See sdkpage.h.

#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/tty.h"
#include "../../sdk/include/sfos.h"

extern "C" const uint8_t sdk_code_start[];
extern "C" const uint8_t sdk_code_end[];
extern "C" const uint8_t sdk_start[];
extern "C" const uint8_t sdk_console_print[];
extern "C" const uint8_t sdk_console_readline[];

namespace
{
    // The data page, as the program sees it.
    struct sdk_data
    {
        SfSystem  Sys;
        SfApp     App;
        SfConsole Console;
        char      Name[64];
    };

    static_assert(sizeof(sdk_data) <= PAGE_SIZE_4K, "the SDK data fits one page");

    // User address of a stub, given where the code page is mapped.
    uint64_t stub(const uint8_t* sym)
    {
        return USER_SDK_CODE + (uint64_t)(sym - sdk_code_start);
    }

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

    void header(SfTableHeader* h, uint64_t signature, uint32_t revision, uint32_t size)
    {
        h->Signature = signature;
        h->Revision  = revision;
        h->Size      = size;
    }
}

namespace sdkpage
{
    bool install(const char* name, Entry* out)
    {
        uint64_t code_len = (uint64_t)(sdk_code_end - sdk_code_start);
        uint8_t* code = map_page(USER_SDK_CODE, 0);             // R + X
        if (!code || code_len > PAGE_SIZE_4K)
            return false;
        memory::memcpy(code, sdk_code_start, code_len);

        sdk_data* d = (sdk_data*)map_page(USER_SDK_DATA, PAGE_NX);  // R
        if (!d)
            return false;
        const uint64_t base = USER_SDK_DATA;

        uint32_t n = 0;
        for (; name[n] && n < sizeof(d->Name) - 1; n++)
            d->Name[n] = name[n];
        d->Name[n] = '\0';

        header(&d->Console.Hdr, SF_CONSOLE_SIGNATURE, SF_CONSOLE_REVISION, sizeof(SfConsole));
        d->Console.Print    = (SfStatus (*)(SfConsole*, const char*))stub(sdk_console_print);
        d->Console.ReadLine = (SfStatus (*)(SfConsole*, char*, uint64_t, uint64_t*))
                              stub(sdk_console_readline);

        header(&d->App.Hdr, SF_APP_SIGNATURE, SF_APP_REVISION, sizeof(SfApp));
        d->App.Name = (const char*)(base + __builtin_offsetof(sdk_data, Name));

        header(&d->Sys.Hdr, SF_SYSTEM_SIGNATURE, SF_SYSTEM_REVISION, sizeof(SfSystem));
        d->Sys.Console = (SfConsole*)(base + __builtin_offsetof(sdk_data, Console));

        out->start = stub(sdk_start);
        out->app   = base + __builtin_offsetof(sdk_data, App);
        out->sys   = base + __builtin_offsetof(sdk_data, Sys);
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
}
