#include "../../include/cpu/ports.h"

namespace port {
    UINT8 byte_in(UINT16 port)
    {
        UINT8 result;
        asm volatile("in %%dx, %%al" : "=a"(result) : "d"(port));
        return result;
    }

    void byte_out(UINT16 port, UINT8 data)
    {
        asm volatile("out %%al, %%dx" : : "a"(data), "d"(port));
    }

    UINT16 word_in(UINT16 port)
    {
        UINT16 result;
        asm volatile("in %%dx, %%ax" : "=a"(result) : "d"(port));
        return result;
    }

    void word_out(UINT16 port, UINT16 data)
    {
        asm volatile("out %%ax, %%dx" : : "a"(data), "d"(port));
    }

    UINT32 dword_in(UINT16 port)
    {
        UINT32 result;
        asm volatile("in %%dx, %%eax" : "=a"(result) : "d"(port));
        return result;
    }

    void dword_out(UINT16 port, UINT32 data)
    {
        asm volatile("out %%eax, %%dx" : : "a"(data), "d"(port));
    }

    void io_wait()
    {
        byte_out(0x80, 0);
    }
} // namespace