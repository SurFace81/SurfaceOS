// The program's start and the SDK tables it gets.

#include "runtime.h"

extern "C" char SdkStateStart[];     // runtime.ld
extern "C" char SdkStateEnd[];

typedef SfStatus (*SfMainFunction)(SfApp* App, SfSystem* Sys);

// The kernel starts the program here (abi/sdkimage.h). SfMain's SfStatus
// becomes the exit status.
extern "C" __attribute__((noreturn)) void SdkStart(SfMainFunction Main);

extern "C" __attribute__((section(".sdk_header"), used))
const SdkHeader SdkImageHeader =
{
    SDK_HEADER_MAGIC,
    (uint64_t)&SdkStart,
    (uint64_t)SdkStateStart,
    (uint64_t)SdkStateEnd,
};

// --- Console ---------------------------------------------------------------

static SfStatus ConsolePrint(SfConsole*, const char* Text)
{
    return SfCall(SFCALL_CONSOLE_PRINT, (uint64_t)Text);
}

static SfStatus ConsoleReadLine(SfConsole*, char* Buffer, uint64_t Size, uint64_t* Length)
{
    return SfCall(SFCALL_CONSOLE_READLINE, (uint64_t)Buffer, Size, (uint64_t)Length);
}

// --- The tables --------------------------------------------------------------
// Constant: they sit on the SDK's code pages, which the program can only
// read.

static const SfConsole SdkConsole =
{
    { SF_CONSOLE_SIGNATURE, SF_CONSOLE_REVISION, sizeof(SfConsole) },
    ConsolePrint,
    ConsoleReadLine,
};

const SfFiles SdkFiles =
{
    { SF_FILES_SIGNATURE, SF_FILES_REVISION, sizeof(SfFiles) },
    FilesOpen,
    FilesCreateUnique,
};

const SfMemory SdkMemory =
{
    { SF_MEMORY_SIGNATURE, SF_MEMORY_REVISION, sizeof(SfMemory) },
    MemoryAllocatePages,
    MemoryFreePages,
    MemoryAllocate,
    MemoryFree,
};

static const SfApp SdkApp =
{
    { SF_APP_SIGNATURE, SF_APP_REVISION, sizeof(SfApp) },
    (const char*)(SDK_INFO_ADDRESS + SF_OFFSET_OF(SdkStartInfo, Name)),
};

const SfSystem SdkSystem =
{
    { SF_SYSTEM_SIGNATURE, SF_SYSTEM_REVISION, sizeof(SfSystem) },
    (SfConsole*)&SdkConsole,
    (SfFiles*)&SdkFiles,
    (SfMemory*)&SdkMemory,
};

extern "C" void SdkStart(SfMainFunction Main)
{
    SfStatus Status = Main((SfApp*)&SdkApp, (SfSystem*)&SdkSystem);
    SfCall(SFCALL_EXIT, Status);
    __builtin_unreachable();            // SFCALL_EXIT does not return
}
