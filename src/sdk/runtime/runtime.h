#ifndef SDK_RUNTIME_H
#define SDK_RUNTIME_H

// Inside the SDK runtime (see abi/sdkimage.h): the code behind the SDK
// tables. It runs in the program, in ring 3, and reaches the kernel with
// the `syscall` instruction (abi/sfcall.h).

#include <sfos.h>
#include <abi/sdkimage.h>

// One kernel call: number and up to four arguments, SfStatus back.
static inline SfStatus SfCall(uint64_t Number, uint64_t A1 = 0, uint64_t A2 = 0,
                              uint64_t A3 = 0, uint64_t A4 = 0, uint64_t A5 = 0,
                              uint64_t A6 = 0)
{
    SfStatus Result;
    register uint64_t R10 asm("r10") = A4;
    register uint64_t R8 asm("r8") = A5;
    register uint64_t R9 asm("r9") = A6;
    asm volatile("syscall"
                 : "=a"(Result)
                 : "a"(Number), "D"(A1), "S"(A2), "d"(A3), "r"(R10), "r"(R8), "r"(R9)
                 : "rcx", "r11", "memory");
    return Result;
}

// The tables, as SfMain gets them (start.cpp).
extern const SfSystem SdkSystem;
extern const SfMemory SdkMemory;
extern const SfFiles  SdkFiles;

// A lock between the threads of the program (sync.cpp): a count of who
// wants it and an auto-reset event the waiters sleep on. Taking a free
// lock and letting go of one nobody waits for never enter the kernel.
struct SdkLock
{
    volatile sint64_t Count;
    uint64_t          Event;        // handle
};

SfStatus LockInit(SdkLock* Lock);
void     LockAcquire(SdkLock* Lock);
void     LockRelease(SdkLock* Lock);

// sync.cpp
SfStatus SyncCreateMutex(SfSync* This, SfMutex** Out);
SfStatus SyncCreateEvent(SfSync* This, uint64_t Flags, SfEvent** Out);
SfStatus SyncWaitAny(SfSync* This, uint64_t Count, const SfWaitItem* Items, uint64_t TimeoutMs,
                     uint64_t* Index);

// memory.cpp: set up the heap's lock. Before SfMain.
SfStatus MemoryInit();

// memory.cpp
SfStatus MemoryAllocatePages(SfMemory* This, uint64_t Count, void** Address);
SfStatus MemoryFreePages(SfMemory* This, void* Address, uint64_t Count);
SfStatus MemoryAllocate(SfMemory* This, uint64_t Size, void** Buffer);
SfStatus MemoryFree(SfMemory* This, void* Buffer);
SfStatus MemoryReallocate(SfMemory* This, void* Buffer, uint64_t Size, void** NewBuffer);

// file.cpp
SfStatus FilesOpen(SfFiles* This, const char* Path, uint64_t Mode, SfFile** Out);
SfStatus FilesCreateUnique(SfFiles* This, SfFile** Out, char* Path, uint64_t PathSize);
SfStatus FilesCreateDirectory(SfFiles* This, const char* Path);
SfStatus FilesDelete(SfFiles* This, const char* Path);
SfStatus FilesRename(SfFiles* This, const char* OldPath, const char* NewPath);
// The kernel's handle behind an SfFile.
uint64_t FileHandle(SfFile* File);

// The compiler may call these for struct copies and zeroing (memory.cpp).
extern "C" void* memset(void* Dest, int Value, uint64_t Size);
extern "C" void* memcpy(void* Dest, const void* Src, uint64_t Size);

#endif // SDK_RUNTIME_H
