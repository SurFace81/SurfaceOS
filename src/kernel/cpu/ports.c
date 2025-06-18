#include "../../include/cpu/ports.h"

UINT8 port_byte_in(UINT16 port)
{
    UINT8 result;
    asm volatile("in %%dx, %%al" : "=a"(result) : "d"(port));
    return result;
}

void port_byte_out(UINT16 port, UINT8 data)
{
    asm volatile("out %%al, %%dx" : : "a"(data), "d"(port));
}

UINT16 port_word_in(UINT16 port)
{
    UINT16 result;
    asm volatile("in %%dx, %%ax" : "=a"(result) : "d"(port));
    return result;
}

void port_word_out(UINT16 port, UINT16 data)
{
    asm volatile("out %%ax, %%dx" : : "a"(data), "d"(port));
}

void io_wait()
{
    port_byte_out(0x80, 0);
}