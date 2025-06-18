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

#ifdef __cplusplus
extern "C" {
#endif

int initUART(UINT16 port);
void uart_write(const char *str);
char uart_read();
void uart_log_uint16(UINT16 num);
void uart_log_uint64_hex(UINT64 num);
void uart_log_uint64_dec(UINT64 num);

#ifdef __cplusplus
}
#endif

#endif // UART_H