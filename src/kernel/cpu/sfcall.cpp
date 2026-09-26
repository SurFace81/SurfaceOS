// The SurfaceOS ABI: `syscall` instruction setup and dispatch. See sfcall.h.

#include "../../include/cpu/sfcall.h"
#include "../../include/cpu/process.h"
#include "../../include/cpu/sdkpage.h"
#include "../../include/cpu/sffile.h"
#include "../../include/drivers/uart.h"

namespace
{
    const uint32_t MSR_EFER   = 0xC0000080;
    const uint32_t MSR_STAR   = 0xC0000081;
    const uint32_t MSR_LSTAR  = 0xC0000082;
    const uint32_t MSR_SFMASK = 0xC0000084;

    const uint64_t EFER_SCE   = 1ULL << 0;      // syscall/sysret enable

    // Cleared on entry: interrupts until the entry is on the kernel stack,
    // and the flags the kernel must not inherit from user code.
    const uint64_t RFLAGS_TF  = 1ULL << 8;
    const uint64_t RFLAGS_IF  = 1ULL << 9;
    const uint64_t RFLAGS_DF  = 1ULL << 10;
    const uint64_t RFLAGS_AC  = 1ULL << 18;

    const uint64_t KERNEL_CS  = 0x08;

    const uint32_t SFCALL_TABLE_SIZE = 64;
    sfcall::handler_t table[SFCALL_TABLE_SIZE];

    inline uint64_t rdmsr(uint32_t msr)
    {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }

    inline void wrmsr(uint32_t msr, uint64_t v)
    {
        asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    }
}

namespace sfcall
{
    void init()
    {
        // STAR[47:32]: the kernel CS the instruction loads (SS = CS + 8).
        // The return path is iretq, not sysret, so STAR[63:48] is unused.
        wrmsr(MSR_STAR, KERNEL_CS << 32);
        wrmsr(MSR_LSTAR, (uint64_t)sfcall_entry);
        wrmsr(MSR_SFMASK, RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_AC);
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

        set_handler(SFCALL_EXIT, process::sf_exit);
        set_handler(SFCALL_CONSOLE_PRINT, sdkpage::console_print);
        set_handler(SFCALL_CONSOLE_READLINE, sdkpage::console_readline);
        sffile::init();
        uart::printf("boot: syscall instruction enabled\n");
    }

    void set_handler(uint32_t nr, handler_t h)
    {
        if (nr < SFCALL_TABLE_SIZE)
            table[nr] = h;
    }
}

// Called by sfcall_entry with the saved registers; the iret frame sits
// right above them.
extern "C" void sfcall_dispatch(user_regs* regs)
{
    iret_frame* iret = (iret_frame*)(regs + 1);
    uint64_t nr = regs->rax;

    process::syscall_enter(nr);

    sfcall::handler_t h = nr < SFCALL_TABLE_SIZE ? table[nr] : nullptr;
    if (h)
        h(regs, iret);
    else
        regs->rax = SF_UNSUPPORTED;

    process::syscall_return(regs, iret);
}
