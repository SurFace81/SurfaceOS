// The SDK pages of a SurfaceOS program. See sdkpage.h.

#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/paging.h"
#include "../../include/obj/object.h"
#include "../../include/cpu/uaccess.h"
#include "../../include/mm/pmm.h"
#include "../../include/mm/heap.h"
#include "../../include/mm/memory.h"
#include "../../include/drivers/tty.h"
#include "../../sdk/include/abi/errno.h"
#include "../../sdk/include/sfos.h"

extern "C" const uint8_t sdk_code_start[];
extern "C" const uint8_t sdk_code_end[];
extern "C" const uint8_t sdk_start[];
extern "C" const uint8_t sdk_console_print[];
extern "C" const uint8_t sdk_console_readline[];
extern "C" const uint8_t sdk_files_open[];
extern "C" const uint8_t sdk_files_create_unique[];
extern "C" const uint8_t sdk_file_open[];
extern "C" const uint8_t sdk_file_close[];
extern "C" const uint8_t sdk_file_read[];
extern "C" const uint8_t sdk_file_write[];
extern "C" const uint8_t sdk_file_get_position[];
extern "C" const uint8_t sdk_file_set_position[];

namespace
{
    // The code page every process maps (PAGE_SHARED).
    uint64_t code_frame = 0;

    // The data page, as the program sees it.
    struct sdk_data
    {
        SfSystem  Sys;
        SfApp     App;
        SfConsole Console;
        SfFiles   Files;
        char      Name[64];
    };

    static_assert(sizeof(sdk_data) <= PAGE_SIZE_4K, "the SDK data fits one page");
    static_assert(HANDLE_TABLE_SIZE * sizeof(SfFile) == PAGE_SIZE_4K,
                  "one SfFile table per handle slot fills the files page");

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
    void init()
    {
        uint64_t code_len = (uint64_t)(sdk_code_end - sdk_code_start);
        uint64_t frame = pmm::alloc_frame();
        if (!frame || code_len > PAGE_SIZE_4K)
            return;                 // install() then fails every program
        uint8_t* code = (uint8_t*)phys_to_virt(frame);
        memory::memset(code, 0x00, PAGE_SIZE_4K);
        memory::memcpy(code, sdk_code_start, code_len);
        code_frame = frame;
    }

    bool install(const char* name, Entry* out)
    {
        // R + X, and not the process's own frame.
        if (!code_frame || !paging::map_user_page(USER_SDK_CODE, code_frame, PAGE_SHARED))
            return false;

        sdk_data* d = (sdk_data*)map_page(USER_SDK_DATA, PAGE_NX);  // R
        SfFile* files = (SfFile*)map_page(USER_SDK_FILES, PAGE_NX); // R
        if (!d || !files)
            return false;

        // One SfFile table per handle slot, all alike (sffile.h).
        for (uint32_t h = 0; h < HANDLE_TABLE_SIZE; h++)
        {
            SfFile* f = &files[h];
            header(&f->Hdr, SF_FILE_SIGNATURE, SF_FILE_REVISION, sizeof(SfFile));
            f->Open        = (SfStatus (*)(SfFile*, const char*, uint64_t, SfFile**))
                             stub(sdk_file_open);
            f->Close       = (SfStatus (*)(SfFile*))stub(sdk_file_close);
            f->Read        = (SfStatus (*)(SfFile*, void*, uint64_t*))stub(sdk_file_read);
            f->Write       = (SfStatus (*)(SfFile*, const void*, uint64_t*))stub(sdk_file_write);
            f->GetPosition = (SfStatus (*)(SfFile*, uint64_t*))stub(sdk_file_get_position);
            f->SetPosition = (SfStatus (*)(SfFile*, uint64_t))stub(sdk_file_set_position);
        }
        const uint64_t base = USER_SDK_DATA;

        uint32_t n = 0;
        for (; name[n] && n < sizeof(d->Name) - 1; n++)
            d->Name[n] = name[n];
        d->Name[n] = '\0';

        header(&d->Console.Hdr, SF_CONSOLE_SIGNATURE, SF_CONSOLE_REVISION, sizeof(SfConsole));
        d->Console.Print    = (SfStatus (*)(SfConsole*, const char*))stub(sdk_console_print);
        d->Console.ReadLine = (SfStatus (*)(SfConsole*, char*, uint64_t, uint64_t*))
                              stub(sdk_console_readline);

        header(&d->Files.Hdr, SF_FILES_SIGNATURE, SF_FILES_REVISION, sizeof(SfFiles));
        d->Files.Open         = (SfStatus (*)(SfFiles*, const char*, uint64_t, SfFile**))
                                stub(sdk_files_open);
        d->Files.CreateUnique = (SfStatus (*)(SfFiles*, SfFile**, char*, uint64_t))
                                stub(sdk_files_create_unique);

        header(&d->App.Hdr, SF_APP_SIGNATURE, SF_APP_REVISION, sizeof(SfApp));
        d->App.Name = (const char*)(base + __builtin_offsetof(sdk_data, Name));

        header(&d->Sys.Hdr, SF_SYSTEM_SIGNATURE, SF_SYSTEM_REVISION, sizeof(SfSystem));
        d->Sys.Console = (SfConsole*)(base + __builtin_offsetof(sdk_data, Console));
        d->Sys.Files   = (SfFiles*)(base + __builtin_offsetof(sdk_data, Files));

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
