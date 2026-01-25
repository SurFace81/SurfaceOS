#include "../../include/cpu/ports.h"

namespace port {
    uint8_t byte_in(uint16_t port)
    {
        uint8_t result;
        asm volatile("in %%dx, %%al" : "=a"(result) : "d"(port));
        return result;
    }

    void byte_out(uint16_t port, uint8_t data)
    {
        asm volatile("out %%al, %%dx" : : "a"(data), "d"(port));
    }

    uint16_t word_in(uint16_t port)
    {
        uint16_t result;
        asm volatile("in %%dx, %%ax" : "=a"(result) : "d"(port));
        return result;
    }

    void word_out(uint16_t port, uint16_t data)
    {
        asm volatile("out %%ax, %%dx" : : "a"(data), "d"(port));
    }

    uint32_t dword_in(uint16_t port)
    {
        uint32_t result;
        asm volatile("in %%dx, %%eax" : "=a"(result) : "d"(port));
        return result;
    }

    void dword_out(uint16_t port, uint32_t data)
    {
        asm volatile("out %%eax, %%dx" : : "a"(data), "d"(port));
    }

    void io_wait()
    {
        byte_out(0x80, 0);
    }
} // namespace