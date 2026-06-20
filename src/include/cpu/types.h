#ifndef TYPES_H
#define TYPES_H

typedef unsigned long long      uint64_t;
typedef          long long      sint64_t;

typedef unsigned int            uint32_t;
typedef          int            sint32_t;

typedef unsigned short          uint16_t;
typedef          short          sint16_t;

typedef unsigned char           uint8_t;
typedef          char           sint8_t;

#define NULL (void*)0

#if defined(__x86_64__) || defined(_M_X64)  // x86_64
    typedef uint64_t uintptr_t;
    typedef uint64_t size_t;
#else  // x86
    typedef uint32_t uintptr_t;
    typedef uint32_t size_t;
#endif

#ifndef __cplusplus
typedef enum {false, true} bool;
#endif

#endif  // TYPES_H