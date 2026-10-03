#ifndef ABI_TYPES_H
#define ABI_TYPES_H

// The scalar types of the SDK and the runtime.

typedef unsigned long long  uint64_t;
typedef          long long  sint64_t;
typedef unsigned int        uint32_t;
typedef          int        sint32_t;
typedef unsigned short      uint16_t;
typedef          short      sint16_t;
typedef unsigned char       uint8_t;
typedef          char       sint8_t;
typedef uint64_t            size_t;
typedef uint64_t            uintptr_t;

// bool, true and false in C as C++ has them (C23 has them built in).
#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
typedef _Bool bool;
#define true    1
#define false   0
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr        // ((void*)0) does not convert to other pointers in C++
#else
#define NULL ((void*)0)
#endif
#endif

#endif