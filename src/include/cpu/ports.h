#ifndef PORTS_H
#define PORTS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

UINT8 port_byte_in(UINT16 port);
void port_byte_out(UINT16 port, UINT8 data);
UINT16 port_word_in(UINT16 port);
void port_word_out(UINT16 port, UINT16 data);
UINT32 port_dword_in(UINT16 port);
void port_dword_out(UINT16 port, UINT32 data);

#ifdef __cplusplus
}
#endif

#endif  // PORTS_H
