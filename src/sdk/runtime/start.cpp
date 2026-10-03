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

static SfStatus ConsoleClear(SfConsole*)
{
    return SfCall(SFCALL_CONSOLE_CLEAR);
}

static SfStatus ConsoleWaitInput(SfConsole*)
{
    return SfCall(SFCALL_CONSOLE_WAIT_INPUT);
}

static SfStatus ConsoleSetHints(SfConsole*, const char* Commands, const char* Names)
{
    return SfCall(SFCALL_CONSOLE_SET_HINTS, (uint64_t)Commands, (uint64_t)Names);
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

// The kernel takes the argument files as handle numbers (~0: none).
static SfStatus ProcessStart(SfProcess*, const char* Name, uint64_t ArgCount,
                             const char* const* Args, SfFile* const* ArgFiles, uint64_t Flags,
                             uint64_t* Handle)
{
    // SF_START_OUTPUT: the output file follows the arguments' files.
    const uint64_t MaxArgFiles = 32;
    uint64_t Handles[MaxArgFiles + 1];
    if ((Flags & SF_START_OUTPUT) && (!ArgFiles || !ArgFiles[ArgCount]))
        return SF_INVALID_PARAMETER;
    if (ArgFiles)
    {
        if (ArgCount > MaxArgFiles)
            return SF_INVALID_PARAMETER;
        uint64_t Files = ArgCount + ((Flags & SF_START_OUTPUT) ? 1 : 0);
        for (uint64_t i = 0; i < Files; i++)
            Handles[i] = ArgFiles[i] ? FileHandle(ArgFiles[i]) : ~0ULL;
    }
    return SfCall(SFCALL_PROCESS_START, (uint64_t)Name, ArgCount, (uint64_t)Args,
                  (uint64_t)Handle, Flags, ArgFiles ? (uint64_t)Handles : 0);
}

static SfStatus ProcessWait(SfProcess*, uint64_t Handle, SfStatus* Status)
{
    return SfCall(SFCALL_PROCESS_WAIT, Handle, (uint64_t)Status);
}

static SfStatus ProcessIdOf(SfProcess*, uint64_t Handle, uint64_t* Id)
{
    return SfCall(SFCALL_PROCESS_ID_OF, Handle, (uint64_t)Id);
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
    ConsoleClear,
    ConsoleWaitInput,
    ConsoleSetHints,
};

const SfFiles SdkFiles =
{
    { SF_FILES_SIGNATURE, SF_FILES_REVISION, sizeof(SfFiles) },
    FilesOpen,
    FilesCreateUnique,
    FilesCreateDirectory,
    FilesDelete,
    FilesRename,
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
    ProcessIdOf,
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

// --- Admin -----------------------------------------------------------------

static SfStatus AdminListProcesses(SfAdmin*, SfProcessInfo* Buffer, uint64_t* Count)
{
    return SfCall(SFCALL_ADMIN_LIST_PROCESSES, (uint64_t)Buffer, (uint64_t)Count);
}

static SfStatus AdminEndProcess(SfAdmin*, uint64_t Id)
{
    return SfCall(SFCALL_ADMIN_END_PROCESS, Id);
}

static SfStatus AdminMount(SfAdmin*, const char* Device)
{
    return SfCall(SFCALL_ADMIN_MOUNT, (uint64_t)Device);
}

static SfStatus AdminUnmount(SfAdmin*, const char* Device)
{
    return SfCall(SFCALL_ADMIN_UNMOUNT, (uint64_t)Device);
}

static SfStatus AdminRestart(SfAdmin*)
{
    return SfCall(SFCALL_ADMIN_RESTART);
}

static SfStatus AdminShutDown(SfAdmin*)
{
    return SfCall(SFCALL_ADMIN_SHUT_DOWN);
}

static SfStatus AdminSync(SfAdmin*)
{
    return SfCall(SFCALL_ADMIN_SYNC);
}

static SfStatus AdminSetTime(SfAdmin*, const SfDateTime* Time)
{
    return SfCall(SFCALL_ADMIN_SET_TIME, (uint64_t)Time);
}

static SfStatus AdminReport(SfAdmin*, const char* Topic, char* Buffer, uint64_t* Size)
{
    return SfCall(SFCALL_ADMIN_REPORT, (uint64_t)Topic, (uint64_t)Buffer, (uint64_t)Size);
}

static SfStatus AdminForeground(SfAdmin*, uint64_t Id)
{
    return SfCall(SFCALL_ADMIN_FOREGROUND, Id);
}

static SfStatus AdminBackground(SfAdmin*, uint64_t Id)
{
    return SfCall(SFCALL_ADMIN_BACKGROUND, Id);
}

static SfStatus AdminGetSystemInfo(SfAdmin*, SfSystemInfo* Info)
{
    return SfCall(SFCALL_ADMIN_GET_SYSTEM_INFO, (uint64_t)Info);
}

static SfStatus AdminGetProcessInfo(SfAdmin*, uint64_t Id, SfProcessStats* Info)
{
    return SfCall(SFCALL_ADMIN_GET_PROCESS_INFO, Id, (uint64_t)Info);
}

static const SfAdmin SdkAdmin =
{
    { SF_ADMIN_SIGNATURE, SF_ADMIN_REVISION, sizeof(SfAdmin) },
    AdminListProcesses,
    AdminEndProcess,
    AdminMount,
    AdminUnmount,
    AdminRestart,
    AdminShutDown,
    AdminSync,
    AdminSetTime,
    AdminReport,
    AdminForeground,
    AdminBackground,
    AdminGetSystemInfo,
    AdminGetProcessInfo,
};

// Filled in by SdkStart from the start info.
static SfApp SdkApp;

// Two system tables: a program with the admin right gets the one with
// Admin (SDK_START_ADMIN), everyone else the one without.
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
    nullptr,
};

const SfSystem SdkAdminSystem =
{
    { SF_SYSTEM_SIGNATURE, SF_SYSTEM_REVISION, sizeof(SfSystem) },
    (SfConsole*)&SdkConsole,
    (SfFiles*)&SdkFiles,
    (SfMemory*)&SdkMemory,
    (SfTime*)&SdkTime,
    (SfProcess*)&SdkProcess,
    (SfThread*)&SdkThread,
    (SfSync*)&SdkSync,
    (SfAdmin*)&SdkAdmin,
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
        Status = Main((SfApp*)&SdkApp, (SfSystem*)((Info->Flags & SDK_START_ADMIN)
                                                   ? &SdkAdminSystem : &SdkSystem));
    SfCall(SFCALL_EXIT, Status);
    __builtin_unreachable();            // SFCALL_EXIT does not return
}
