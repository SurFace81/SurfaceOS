#ifndef TYPES_H
#define TYPES_H

typedef unsigned long long      UINT64;
typedef          long long      SINT64;

typedef unsigned int            UINT32;
typedef          int            SINT32;

typedef unsigned short          UINT16;
typedef          short          SINT16;

typedef unsigned char           UINT8;
typedef          char           SINT8;

#define NULL (void*)0

#if defined(__x86_64__) || defined(_M_X64)  // x86_64
    typedef UINT64 uintptr_t;
#else  // x86
    typedef UINT32 uintptr_t;
#endif

#ifndef __cplusplus
typedef enum {false, true} bool;
#endif

#endif  // TYPES_H