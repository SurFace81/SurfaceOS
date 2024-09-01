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

#ifndef __cplusplus
typedef enum {false, true} bool;
#endif

#endif