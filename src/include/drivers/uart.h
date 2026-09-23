#ifndef UART_H
#define UART_H

#include "../cpu/ports.h"
#include "../cpu/types.h"
#include "../cpu/pci.h"

// Bytes of boot log kept for `dmesg` when there is no serial port to read.
#define UART_LOG_SIZE 8192

namespace uart {
    int  init();
    uint32_t log_read(char* out, uint32_t max);
    //void listen_loop();
    void printf(const char* fmt, ...);
    void write(const char* s, uint64_t len);   // raw bytes, NUL included
}

#endif