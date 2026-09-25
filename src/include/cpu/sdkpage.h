#ifndef SDKPAGE_H
#define SDKPAGE_H

#include "types.h"

// The SDK pages every program is started with.
//
//   USER_SDK_CODE   read + execute: the SDK's code - the start-up stub that
//                   calls SfMain and ends the process, and the protocol
//                   functions (Console->Print, ...), each a `syscall`.
//                   Position-independent, copied from the kernel image
//                   (sdkpage.asm) into every such process.
//   USER_SDK_DATA   read-only: the tables SfMain gets - SfSystem, SfApp,
//                   SfConsole - filled in by the kernel for this process.
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

    // Map and fill both pages in the current address space for program
    // `name`. false when out of memory (the caller destroys the space).
    bool install(const char* name, Entry* out);

    // SFCALL_CONSOLE_PRINT (Text): write a NUL-terminated string to the
    // console.
    void console_print(user_regs* regs, iret_frame* iret);
}

#endif // SDKPAGE_H
