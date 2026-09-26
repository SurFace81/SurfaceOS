#ifndef SDK_RUNTIME_H
#define SDK_RUNTIME_H

// Inside the SDK runtime (see abi/sdkimage.h): the code behind the SDK
// tables. It runs in the program, in ring 3, and reaches the kernel with
// the `syscall` instruction (abi/sfcall.h).

#include <sfos.h>
#include <abi/sdkimage.h>

// One kernel call: number and up to four arguments, SfStatus back.
static inline SfStatus SfCall(uint64_t Number, uint64_t A1 = 0, uint64_t A2 = 0,
                              uint64_t A3 = 0, uint64_t A4 = 0)
{
    SfStatus Result;
    register uint64_t R10 asm("r10") = A4;
    asm volatile("syscall"
                 : "=a"(Result)
                 : "a"(Number), "D"(A1), "S"(A2), "d"(A3), "r"(R10)
                 : "rcx", "r11", "memory");
    return Result;
}

// The tables, as SfMain gets them (start.cpp).
extern const SfSystem SdkSystem;
extern const SfMemory SdkMemory;
extern const SfFiles  SdkFiles;

// memory.cpp
SfStatus MemoryAllocatePages(SfMemory* This, uint64_t Count, void** Address);
SfStatus MemoryFreePages(SfMemory* This, void* Address, uint64_t Count);
SfStatus MemoryAllocate(SfMemory* This, uint64_t Size, void** Buffer);
SfStatus MemoryFree(SfMemory* This, void* Buffer);

// file.cpp
SfStatus FilesOpen(SfFiles* This, const char* Path, uint64_t Mode, SfFile** Out);
SfStatus FilesCreateUnique(SfFiles* This, SfFile** Out, char* Path, uint64_t PathSize);

// The compiler may call these for struct copies and zeroing (memory.cpp).
extern "C" void* memset(void* Dest, int Value, uint64_t Size);
extern "C" void* memcpy(void* Dest, const void* Src, uint64_t Size);

#endif // SDK_RUNTIME_H
