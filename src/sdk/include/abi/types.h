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
typedef sint64_t            ssize_t;
typedef uint64_t            uintptr_t;

// POSIX-shaped scalar types (Linux x86_64 widths).
typedef sint64_t            off_t;
typedef uint32_t            mode_t;
typedef uint32_t            uid_t;
typedef uint32_t            gid_t;
typedef sint32_t            pid_t_abi;      // pid_t itself lives in abi/process.h
typedef uint64_t            dev_t;
typedef uint64_t            ino_t;
typedef uint64_t            nlink_t;
typedef sint64_t            blksize_t;
typedef sint64_t            blkcnt_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr        // ((void*)0) does not convert to other pointers in C++
#else
#define NULL ((void*)0)
#endif
#endif

#endif