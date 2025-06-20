#ifndef PORTS_H
#define PORTS_H

#include "types.h"

namespace port {
    UINT8  byte_in  (UINT16 port);
    void   byte_out (UINT16 port, UINT8 data);
    UINT16 word_in  (UINT16 port);
    void   word_out (UINT16 port, UINT16 data);
    UINT32 dword_in (UINT16 port);
    void   dword_out(UINT16 port, UINT32 data);

    void   io_wait(void);
}

#endif  // PORTS_H
