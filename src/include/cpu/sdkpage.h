#ifndef SDKPAGE_H
#define SDKPAGE_H

#include "types.h"

// The SDK pages every program is started with.
//
//   USER_SDK_CODE   read + execute: the SDK's code - the start-up stub that
//                   calls SfMain and ends the process, and the protocol
//                   functions (Console->Print, ...), each a `syscall`.
//                   One frame for the whole system, filled from the kernel
//                   image (sdkpage.asm) at boot and mapped PAGE_SHARED into
//                   every process.
//   USER_SDK_DATA   read-only: the tables SfMain gets - SfSystem, SfApp,
//                   SfConsole, SfFiles - filled in by the kernel for this
//                   process.
//   USER_SDK_FILES  read-only: one SfFile table per handle slot (sffile.h).
//
// The program never writes either page; the kernel fills them through the
// direct map before the process first runs.

struct user_regs;
struct iret_frame;

namespace sdkpage
{
    struct Entry
    {
        uint64_t start;     // where the process starts (the stub)
        uint64_t app;       // SfApp* for SfMain
        uint64_t sys;       // SfSystem* for SfMain
    };

    // Set up the shared code frame. Called once from kmain.
    void init();

    // Map the three pages in the current address space and fill the data page
    // for program `name`. false when out of memory (the caller destroys
    // the space).
    bool install(const char* name, Entry* out);

    // SFCALL_CONSOLE_PRINT (Text): write a NUL-terminated string to the
    // console.
    void console_print(user_regs* regs, iret_frame* iret);

    // SFCALL_CONSOLE_READLINE (Buffer, Size, Length): sleep until the user
    // types a line and copy it without the line break, NUL-terminated and
    // cut to Size - 1 bytes. Length may be null.
    void console_readline(user_regs* regs, iret_frame* iret);
}

#endif // SDKPAGE_H
