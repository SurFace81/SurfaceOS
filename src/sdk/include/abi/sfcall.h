#ifndef ABI_SFCALL_H
#define ABI_SFCALL_H

// The SurfaceOS ABI: entered with the `syscall` instruction.
//
//   rax        call number (SFCALL_*)
//   rdi, rsi, rdx, r10, r8, r9
//              arguments
//   rax        result: an SfStatus (below)
//
// rcx and r11 do not survive a call (the instruction itself uses them);
// every other register does.

// SfStatus: 0 is success, the top bit marks an error. Programs see these
// through <sfos.h>; the kernel returns them.
/// The top bit: set in every error SfStatus.
#define SF_ERROR_BIT            0x8000000000000000ULL
/// The call did what it was asked.
#define SF_SUCCESS              0ULL
/// No such call or operation.
#define SF_UNSUPPORTED          (SF_ERROR_BIT | 1)
/// An argument is wrong: null where it may not be, out of range, malformed.
#define SF_INVALID_PARAMETER    (SF_ERROR_BIT | 2)
/// No such file, program, device or other thing.
#define SF_NOT_FOUND            (SF_ERROR_BIT | 3)
/// Not allowed: a path above its root, a call without the admin right.
#define SF_ACCESS_DENIED        (SF_ERROR_BIT | 4)
/// Not enough memory, handles or slots.
#define SF_OUT_OF_RESOURCES     (SF_ERROR_BIT | 5)
/// Cut short: Ctrl+C during ReadLine, a program being ended.
#define SF_ABORTED              (SF_ERROR_BIT | 6)
/// The time to wait ran out.
#define SF_TIMEOUT              (SF_ERROR_BIT | 7)
/// Nothing more to read.
#define SF_END_OF_FILE          (SF_ERROR_BIT | 8)
/// A handle or id that refers to nothing of the kind.
#define SF_BAD_HANDLE           (SF_ERROR_BIT | 9)
/// There is something by that name already.
#define SF_ALREADY_EXISTS       (SF_ERROR_BIT | 10)
/// The device or the volume on it failed.
#define SF_DEVICE_ERROR         (SF_ERROR_BIT | 11)
/// The buffer does not fit the result; the size needed is returned.
#define SF_BUFFER_TOO_SMALL     (SF_ERROR_BIT | 12)
/// Busy: a volume with files open, a folder that is not empty.
#define SF_IN_USE               (SF_ERROR_BIT | 13)
/// A program ended by a CPU exception.
#define SF_CRASHED              (SF_ERROR_BIT | 14)

// Call numbers. The table grows from 0; the SDK runtime makes these calls,
// programs never do directly. A file is its handle number.
#define SFCALL_EXIT                  0   // (SfStatus) - does not return
#define SFCALL_CONSOLE_PRINT         1   // (const char* Text)
#define SFCALL_CONSOLE_READLINE      2   // (char* Buffer, uint64_t Size, uint64_t* Length)
#define SFCALL_FILES_OPEN            3   // (const char* Path, uint64_t Mode, uint64_t* Handle)
#define SFCALL_FILES_CREATE_UNIQUE   4   // (uint64_t* Handle, char* Path, uint64_t PathSize)
#define SFCALL_FILE_OPEN             5   // (uint64_t Dir, const char* Path, uint64_t Mode, uint64_t* Handle)
#define SFCALL_CLOSE                 6   // (uint64_t Handle): any handle
#define SFCALL_FILE_READ             7   // (uint64_t Handle, void* Buffer, uint64_t* Size)
#define SFCALL_FILE_WRITE            8   // (uint64_t Handle, const void* Buffer, uint64_t* Size)
#define SFCALL_FILE_GET_POSITION     9   // (uint64_t Handle, uint64_t* Position)
#define SFCALL_FILE_SET_POSITION     10  // (uint64_t Handle, uint64_t Position)
#define SFCALL_MEMORY_ALLOCATE_PAGES 11  // (uint64_t Count, void** Address)
#define SFCALL_MEMORY_FREE_PAGES     12  // (void* Address, uint64_t Count)
#define SFCALL_TIME_GET              13  // (SfDateTime* Time)
#define SFCALL_TIME_GET_UPTIME       14  // (uint64_t* Milliseconds)
#define SFCALL_TIME_SLEEP            15  // (uint64_t Milliseconds)
#define SFCALL_PROCESS_GET_ID        16  // (uint64_t* Id)
#define SFCALL_PROCESS_GET_ARGS      17  // (uint64_t Id, char* Buffer, uint64_t* Size, uint64_t* Count)
#define SFCALL_THREAD_CREATE         18  // (SfThreadEntry Entry, void* Arg, uint64_t* Id)
#define SFCALL_THREAD_EXIT           19  // (SfStatus) - does not return
#define SFCALL_THREAD_JOIN           20  // (uint64_t Id, SfStatus* Status)
#define SFCALL_EVENT_CREATE          21  // (uint64_t Flags, uint64_t* Handle)
#define SFCALL_EVENT_SET             22  // (uint64_t Handle)
#define SFCALL_EVENT_RESET           23  // (uint64_t Handle)
#define SFCALL_WAIT                  24  // (uint64_t Handle, uint64_t TimeoutMs): any waitable handle
#define SFCALL_PROCESS_START         25  // (const char* Name, uint64_t ArgCount, const char* const* Args, uint64_t* Handle, uint64_t Flags, const uint64_t* ArgHandles)
#define SFCALL_PROCESS_WAIT          26  // (uint64_t Handle, SfStatus* Status)
#define SFCALL_CONSOLE_GET_SIZE      27  // (uint32_t* Columns, uint32_t* Rows)
#define SFCALL_CONSOLE_SET_CURSOR    28  // (uint32_t Column, uint32_t Row, uint8_t Visible)
#define SFCALL_CONSOLE_SET_COLOR     29  // (uint8_t Foreground, uint8_t Background)
#define SFCALL_CONSOLE_WRITE_AT      30  // (uint32_t Column, uint32_t Row, const char* Text)
#define SFCALL_CONSOLE_DRAW          31  // (uint32_t Column, uint32_t Row, uint32_t Width, uint32_t Height, const SfCell* Cells)
#define SFCALL_CONSOLE_READ_KEY      32  // (SfKey* Key)
#define SFCALL_CONSOLE_SET_MODE      33  // (uint64_t Mode)
#define SFCALL_CONSOLE_SET_TITLE     34  // (const char* Text)
#define SFCALL_ADMIN_LIST_PROCESSES  35  // (SfProcessInfo* Buffer, uint64_t* Count)
#define SFCALL_ADMIN_END_PROCESS     36  // (uint64_t Id)
#define SFCALL_ADMIN_MOUNT           37  // (const char* Device)
#define SFCALL_ADMIN_UNMOUNT         38  // (const char* Device)
#define SFCALL_ADMIN_RESTART         39  // ()
#define SFCALL_ADMIN_SHUT_DOWN       40  // ()
#define SFCALL_FILE_READ_DIR         41  // (uint64_t Handle, SfDirEntry* Entry)
#define SFCALL_FILE_GET_INFO         42  // (uint64_t Handle, SfDirEntry* Info)
#define SFCALL_FILES_CREATE_DIRECTORY 43 // (const char* Path)
#define SFCALL_FILES_DELETE          44  // (const char* Path)
#define SFCALL_FILES_RENAME          45  // (const char* OldPath, const char* NewPath)
#define SFCALL_CONSOLE_CLEAR         46  // ()
#define SFCALL_ADMIN_SYNC            47  // ()
#define SFCALL_ADMIN_SET_TIME        48  // (const SfDateTime* Time)
#define SFCALL_ADMIN_REPORT          49  // (const char* Topic, char* Buffer, uint64_t* Size)
#define SFCALL_CONSOLE_WAIT_INPUT    50  // ()
#define SFCALL_ADMIN_FOREGROUND      51  // (uint64_t Id)
#define SFCALL_ADMIN_BACKGROUND      52  // (uint64_t Id)
#define SFCALL_PROCESS_ID_OF         53  // (uint64_t Handle, uint64_t* Id)
#define SFCALL_ADMIN_GET_SYSTEM_INFO 54  // (SfSystemInfo* Info)
#define SFCALL_ADMIN_GET_PROCESS_INFO 55 // (uint64_t Id, SfProcessStats* Info)
#define SFCALL_CONSOLE_SET_HINTS     56  // (const char* Commands, const char* Names)
#define SFCALL_WAIT_ANY              57  // (const uint64_t* Items, uint64_t Count, uint64_t TimeoutMs, uint64_t* Index): Items holds Count pairs, kind (SF_WAIT_*) and handle
#define SFCALL_CONSOLE_SET_CLIPBOARD 58  // (const void* Data, uint64_t Size)
#define SFCALL_CONSOLE_GET_CLIPBOARD 59  // (void* Buffer, uint64_t* Size)
#define SFCALL_COUNT                 60

#endif // ABI_SFCALL_H
