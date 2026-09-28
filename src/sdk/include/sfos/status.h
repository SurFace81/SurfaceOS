#ifndef SFOS_STATUS_H
#define SFOS_STATUS_H

#include "../abi/types.h"
#include "../abi/sfcall.h"      // the SF_* status values

// Every SDK call returns an SfStatus: SF_SUCCESS (0), or a value with the top
// bit set. Results come back through out-parameters.
typedef uint64_t SfStatus;

#define SF_ERROR(s)     (((s) & SF_ERROR_BIT) != 0)

// Compile-time layout checks, in C and C++ alike.
#ifdef __cplusplus
#define SF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define SF_OFFSET_OF(type, field)   __builtin_offsetof(type, field)

#endif // SFOS_STATUS_H
