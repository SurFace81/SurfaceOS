// The program's start and the SDK tables it gets.

#include "runtime.h"

extern "C" char SdkStateStart[];     // runtime.ld
extern "C" char SdkStateEnd[];

typedef SfStatus (*SfMainFunction)(SfApp* App, SfSystem* Sys);

// The kernel starts the program here (abi/sdkimage.h). SfMain's SfStatus
// becomes the exit status.
extern "C" __attribute__((noreturn)) void SdkStart(SfMainFunction Main);

// A created thread starts here (SfThread Create); Entry's SfStatus ends it.
extern "C" __attribute__((noreturn)) void SdkThreadStart(SfThreadEntry Entry, void* Arg);

extern "C" __attribute__((section(".sdk_header"), used))
const SdkHeader SdkImageHeader =
{
    SDK_HEADER_MAGIC,
    (uint64_t)&SdkStart,
    (uint64_t)&SdkThreadStart,
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

static SfStatus ConsoleGetSize(SfConsole*, uint32_t* Columns, uint32_t* Rows)
{
    return SfCall(SFCALL_CONSOLE_GET_SIZE, (uint64_t)Columns, (uint64_t)Rows);
}

static SfStatus ConsoleSetCursor(SfConsole*, uint32_t Column, uint32_t Row, uint8_t Visible)
{
    return SfCall(SFCALL_CONSOLE_SET_CURSOR, Column, Row, Visible);
}

static SfStatus ConsoleSetColor(SfConsole*, uint8_t Foreground, uint8_t Background)
{
    return SfCall(SFCALL_CONSOLE_SET_COLOR, Foreground, Background);
}

static SfStatus ConsoleWriteAt(SfConsole*, uint32_t Column, uint32_t Row, const char* Text)
{
    return SfCall(SFCALL_CONSOLE_WRITE_AT, Column, Row, (uint64_t)Text);
}

static SfStatus ConsoleDraw(SfConsole*, uint32_t Column, uint32_t Row, uint32_t Width,
                            uint32_t Height, const SfCell* Cells)
{
    return SfCall(SFCALL_CONSOLE_DRAW, Column, Row, Width, Height, (uint64_t)Cells);
}

static SfStatus ConsoleReadKey(SfConsole*, SfKey* Key)
{
    return SfCall(SFCALL_CONSOLE_READ_KEY, (uint64_t)Key);
}

static SfStatus ConsoleSetMode(SfConsole*, uint64_t Mode)
{
    return SfCall(SFCALL_CONSOLE_SET_MODE, Mode);
}

static SfStatus ConsoleSetTitle(SfConsole*, const char* Text)
{
    return SfCall(SFCALL_CONSOLE_SET_TITLE, (uint64_t)Text);
}

// --- Time ------------------------------------------------------------------

static SfStatus TimeGetTime(SfTime*, SfDateTime* Time)
{
    return SfCall(SFCALL_TIME_GET, (uint64_t)Time);
}

static SfStatus TimeGetUptime(SfTime*, uint64_t* Milliseconds)
{
    return SfCall(SFCALL_TIME_GET_UPTIME, (uint64_t)Milliseconds);
}

static SfStatus TimeSleep(SfTime*, uint64_t Milliseconds)
{
    return SfCall(SFCALL_TIME_SLEEP, Milliseconds);
}

// --- Process ---------------------------------------------------------------

static SfStatus ProcessGetId(SfProcess*, uint64_t* Id)
{
    return SfCall(SFCALL_PROCESS_GET_ID, (uint64_t)Id);
}

static SfStatus ProcessGetArgs(SfProcess*, uint64_t Id, char* Buffer, uint64_t* Size,
                               uint64_t* Count)
{
    return SfCall(SFCALL_PROCESS_GET_ARGS, Id, (uint64_t)Buffer, (uint64_t)Size,
                  (uint64_t)Count);
}

static SfStatus ProcessStart(SfProcess*, const char* Name, uint64_t ArgCount,
                             const char* const* Args, uint64_t Flags, uint64_t* Handle)
{
    return SfCall(SFCALL_PROCESS_START, (uint64_t)Name, ArgCount, (uint64_t)Args,
                  (uint64_t)Handle, Flags);
}

static SfStatus ProcessWait(SfProcess*, uint64_t Handle, SfStatus* Status)
{
    return SfCall(SFCALL_PROCESS_WAIT, Handle, (uint64_t)Status);
}

// --- Threads -----------------------------------------------------------------

static SfStatus ThreadCreate(SfThread*, SfThreadEntry Entry, void* Arg, uint64_t* Id)
{
    return SfCall(SFCALL_THREAD_CREATE, (uint64_t)Entry, (uint64_t)Arg, (uint64_t)Id);
}

static SfStatus ThreadExit(SfThread*, SfStatus Status)
{
    return SfCall(SFCALL_THREAD_EXIT, Status);
}

static SfStatus ThreadJoin(SfThread*, uint64_t Id, SfStatus* Status)
{
    return SfCall(SFCALL_THREAD_JOIN, Id, (uint64_t)Status);
}

extern "C" void SdkThreadStart(SfThreadEntry Entry, void* Arg)
{
    SfCall(SFCALL_THREAD_EXIT, Entry(Arg));
    __builtin_unreachable();            // SFCALL_THREAD_EXIT does not return
}

// --- The tables --------------------------------------------------------------
// Constant, apart from SfApp: they sit on the SDK's code pages, which the
// program can only read.

static const SfConsole SdkConsole =
{
    { SF_CONSOLE_SIGNATURE, SF_CONSOLE_REVISION, sizeof(SfConsole) },
    ConsolePrint,
    ConsoleReadLine,
    ConsoleGetSize,
    ConsoleSetCursor,
    ConsoleSetColor,
    ConsoleWriteAt,
    ConsoleDraw,
    ConsoleReadKey,
    ConsoleSetMode,
    ConsoleSetTitle,
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

static const SfTime SdkTime =
{
    { SF_TIME_SIGNATURE, SF_TIME_REVISION, sizeof(SfTime) },
    TimeGetTime,
    TimeGetUptime,
    TimeSleep,
};

static const SfProcess SdkProcess =
{
    { SF_PROCESS_SIGNATURE, SF_PROCESS_REVISION, sizeof(SfProcess) },
    ProcessGetId,
    ProcessGetArgs,
    ProcessStart,
    ProcessWait,
};

static const SfThread SdkThread =
{
    { SF_THREAD_SIGNATURE, SF_THREAD_REVISION, sizeof(SfThread) },
    ThreadCreate,
    ThreadExit,
    ThreadJoin,
};

static const SfSync SdkSync =
{
    { SF_SYNC_SIGNATURE, SF_SYNC_REVISION, sizeof(SfSync) },
    SyncCreateMutex,
    SyncCreateEvent,
};

// Filled in by SdkStart from the start info.
static SfApp SdkApp;

const SfSystem SdkSystem =
{
    { SF_SYSTEM_SIGNATURE, SF_SYSTEM_REVISION, sizeof(SfSystem) },
    (SfConsole*)&SdkConsole,
    (SfFiles*)&SdkFiles,
    (SfMemory*)&SdkMemory,
    (SfTime*)&SdkTime,
    (SfProcess*)&SdkProcess,
    (SfThread*)&SdkThread,
    (SfSync*)&SdkSync,
};

extern "C" void SdkStart(SfMainFunction Main)
{
    const SdkStartInfo* Info = (const SdkStartInfo*)SDK_INFO_ADDRESS;
    SdkApp.Hdr      = { SF_APP_SIGNATURE, SF_APP_REVISION, sizeof(SfApp) };
    SdkApp.Name     = Info->Name;
    SdkApp.ArgCount = Info->ArgCount;
    SdkApp.Args     = Info->Args;

    SfStatus Status = MemoryInit();
    if (!SF_ERROR(Status))
        Status = Main((SfApp*)&SdkApp, (SfSystem*)&SdkSystem);
    SfCall(SFCALL_EXIT, Status);
    __builtin_unreachable();            // SFCALL_EXIT does not return
}
