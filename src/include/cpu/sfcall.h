#ifndef SFCALL_H
#define SFCALL_H

#include "types.h"
#include "../../sdk/include/abi/sfcall.h"

// The SurfaceOS ABI entry (the `syscall` instruction) and its dispatch
// table. The entry builds the same trap frame on the process's kernel
// stack as int 0x80 does, so the process layer (scheduling, the way back
// to ring 3) treats both alike. See abi/sfcall.h for the register use.

struct user_regs;
struct iret_frame;

extern "C" void sfcall_entry();

namespace sfcall
{
    // A handler reads its arguments from regs and leaves an SfStatus in
    // regs->rax.
    typedef void (*handler_t)(user_regs* regs, iret_frame* iret);

    // Turn the `syscall` instruction on (EFER.SCE, STAR, LSTAR, SFMASK)
    // and fill the dispatch table. Called once from kmain.
    void init();

    // The MSR part alone, for every other CPU as it starts: the MSRs are
    // each CPU's own.
    void init_cpu();

    // Register the handler for call number nr (< SFCALL_TABLE_SIZE).
    void set_handler(uint32_t nr, handler_t h);
}

#endif // SFCALL_H
