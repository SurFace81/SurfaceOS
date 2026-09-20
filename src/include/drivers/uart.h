#ifndef UART_H
#define UART_H

#include "../cpu/ports.h"
#include "../cpu/types.h"
#include "../cpu/pci.h"

namespace uart {
    int  init();
    //void listen_loop();
    void printf(const char* fmt, ...);
    void write(const char* s, uint64_t len);   // raw bytes, NUL included
}

#endif