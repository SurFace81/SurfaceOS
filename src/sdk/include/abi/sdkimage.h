#ifndef ABI_SDKIMAGE_H
#define ABI_SDKIMAGE_H

#include "types.h"

// The SDK runtime: the code behind the SDK tables (src/sdk/runtime),
// linked to run at SDK_CODE_ADDRESS and built into the kernel. The kernel
// maps it into every program:
//
//   SDK_CODE_ADDRESS   the runtime's code and constant tables; one copy
//                      for the whole system, read + execute
//   SDK_INFO_ADDRESS   SdkStartInfo of this process, read-only
//   SDK_STATE_ADDRESS  the runtime's variables (its .bss), zeroed,
//                      read + write, one copy per process
//
// The program starts in SdkHeader.Start(SfMain) with the stack as right
// after a call.

#define SDK_CODE_ADDRESS    0x00007FFF00000000ULL
#define SDK_CODE_MAX        0x00100000ULL                        // 1 MiB
#define SDK_INFO_ADDRESS    (SDK_CODE_ADDRESS + SDK_CODE_MAX)
#define SDK_STATE_ADDRESS   (SDK_INFO_ADDRESS + SDK_CODE_MAX)   // runtime.ld

#define SDK_HEADER_MAGIC    0x5344534F46535553ULL                // "SUSFOSDS"

// The first bytes of the runtime.
typedef struct SdkHeader
{
    uint64_t Magic;
    uint64_t Start;         // void Start(SfMain)
    uint64_t ThreadStart;   // void ThreadStart(Entry, Arg): a created thread
    uint64_t StateStart;    // SDK_STATE_ADDRESS
    uint64_t StateEnd;      // how much of it the runtime uses
} SdkHeader;

// What the kernel tells the runtime about the process. The argument
// strings and the pointers to them follow it, on the same read-only pages.
typedef struct SdkStartInfo
{
    char               Name[64];    // the program's name, NUL-terminated
    uint64_t           ArgCount;
    const char* const* Args;        // ArgCount strings
} SdkStartInfo;

#endif // ABI_SDKIMAGE_H
