#ifndef PORTS_H
#define PORTS_H

#include "types.h"

namespace port {
    uint8_t  byte_in  (uint16_t port);
    void   byte_out (uint16_t port, uint8_t data);
    uint16_t word_in  (uint16_t port);
    void   word_out (uint16_t port, uint16_t data);
    uint32_t dword_in (uint16_t port);
    void   dword_out(uint16_t port, uint32_t data);

    void   io_wait(void);
}

#endif  // PORTS_H
