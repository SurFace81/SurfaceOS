#ifndef ABI_TYPES_H
#define ABI_TYPES_H

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

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif