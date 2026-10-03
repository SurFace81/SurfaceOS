#ifndef SFOS_LIBC_STDINT_H
#define SFOS_LIBC_STDINT_H

// The C names of the SDK's integer types (abi/types.h), the same types.

#include <abi/types.h>

typedef signed char int8_t;      // sint8_t is char, which C keeps apart
typedef sint16_t int16_t;
typedef sint32_t int32_t;
typedef sint64_t int64_t;
typedef sint64_t intptr_t;

#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define INT64_MAX   9223372036854775807LL
#define INT8_MIN    (-INT8_MAX - 1)
#define INT16_MIN   (-INT16_MAX - 1)
#define INT32_MIN   (-INT32_MAX - 1)
#define INT64_MIN   (-INT64_MAX - 1)
#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

#endif // SFOS_LIBC_STDINT_H
