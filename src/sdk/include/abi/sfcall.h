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
// every other register does. The old ABI (int 0x80) stays for the programs
// written against it and gets no new calls.

// SfStatus: 0 is success, the top bit marks an error. Programs see these
// through <sfos.h>; the kernel returns them.
#define SF_ERROR_BIT            0x8000000000000000ULL
#define SF_SUCCESS              0ULL
#define SF_UNSUPPORTED          (SF_ERROR_BIT | 1)   // no such call or operation
#define SF_INVALID_PARAMETER    (SF_ERROR_BIT | 2)
#define SF_NOT_FOUND            (SF_ERROR_BIT | 3)
#define SF_ACCESS_DENIED        (SF_ERROR_BIT | 4)
#define SF_OUT_OF_RESOURCES     (SF_ERROR_BIT | 5)   // memory, handles, slots
#define SF_ABORTED              (SF_ERROR_BIT | 6)   // e.g. Ctrl+C during ReadLine
#define SF_TIMEOUT              (SF_ERROR_BIT | 7)
#define SF_END_OF_FILE          (SF_ERROR_BIT | 8)
#define SF_BAD_HANDLE           (SF_ERROR_BIT | 9)
#define SF_ALREADY_EXISTS       (SF_ERROR_BIT | 10)
#define SF_DEVICE_ERROR         (SF_ERROR_BIT | 11)
#define SF_BUFFER_TOO_SMALL     (SF_ERROR_BIT | 12)  // the size needed is returned

// Call numbers. The table grows from 0; the SDK runtime makes these calls,
// programs never do directly. A file is its handle number.
#define SFCALL_EXIT                  0   // (SfStatus) - does not return
#define SFCALL_CONSOLE_PRINT         1   // (const char* Text)
#define SFCALL_CONSOLE_READLINE      2   // (char* Buffer, uint64_t Size, uint64_t* Length)
#define SFCALL_FILES_OPEN            3   // (const char* Path, uint64_t Mode, uint64_t* Handle)
#define SFCALL_FILES_CREATE_UNIQUE   4   // (uint64_t* Handle, char* Path, uint64_t PathSize)
#define SFCALL_FILE_OPEN             5   // (uint64_t Dir, const char* Path, uint64_t Mode, uint64_t* Handle)
#define SFCALL_FILE_CLOSE            6   // (uint64_t Handle)
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
#define SFCALL_COUNT                 18

#endif // ABI_SFCALL_H
