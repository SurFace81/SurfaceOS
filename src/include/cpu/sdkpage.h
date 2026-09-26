#ifndef SDKPAGE_H
#define SDKPAGE_H

#include "types.h"

// The SDK pages every program is started with (abi/sdkimage.h).
//
//   code    the SDK runtime (src/sdk/runtime): its code and its constant
//           tables. One set of frames for the whole system, copied from
//           the kernel image (sdkpage.asm) at boot and mapped PAGE_SHARED,
//           read + execute, into every process.
//   info    SdkStartInfo of this process and its arguments, read-only.
//   state   the runtime's variables, zeroed, read + write.
//
// The console calls of the runtime end up here too.

struct user_regs;
struct iret_frame;

namespace sdkpage
{
    // Check the runtime and set up the shared code frames. Called once
    // from kmain.
    void init();

    // Map the SDK pages of program `name` in the current address space.
    // `args` holds `argc` NUL-terminated strings back to back, `args_size`
    // bytes. *start gets where the process starts. false when out of
    // memory (the caller destroys the space).
    bool install(const char* name, const char* args, uint32_t args_size, uint32_t argc,
                 uint64_t* start);

    // Where a created thread starts (SdkHeader.ThreadStart), 0 when the
    // runtime is unusable.
    uint64_t thread_start();

    // SFCALL_CONSOLE_PRINT (Text): write a NUL-terminated string to the
    // console.
    void console_print(user_regs* regs, iret_frame* iret);

    // SFCALL_CONSOLE_READLINE (Buffer, Size, Length): sleep until the user
    // types a line and copy it without the line break, NUL-terminated and
    // cut to Size - 1 bytes. Length may be null.
    void console_readline(user_regs* regs, iret_frame* iret);
}

#endif // SDKPAGE_H
