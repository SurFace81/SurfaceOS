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

// SfStatus: 0 is success, the top bit marks an error.
#define SF_ERROR_BIT        0x8000000000000000ULL
#define SF_SUCCESS          0ULL
#define SF_UNSUPPORTED      (SF_ERROR_BIT | 1)   // no such call

// Call numbers: none yet. The table grows from 0.
#define SFCALL_COUNT        0

#endif // ABI_SFCALL_H
