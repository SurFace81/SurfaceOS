#ifndef UART_H
#define UART_H

#include "../cpu/ports.h"
#include "../cpu/types.h"

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8
#define COM5 0x5F8
#define COM6 0x4F8
#define COM7 0x5E8
#define COM8 0x4E8

namespace uart {
    int  init(UINT16 port);
    void write(const char *str);
    char read(void);
    void log_uint16(UINT16 num);
    void log_uint64_hex(UINT64 num);
    void log_uint64_dec(UINT64 num);
}

#endif // UART_H