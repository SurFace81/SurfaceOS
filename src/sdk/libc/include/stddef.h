#ifndef SFOS_LIBC_STDDEF_H
#define SFOS_LIBC_STDDEF_H

#include <abi/types.h>

/// The difference of two pointers.
typedef sint64_t ptrdiff_t;

/// The byte offset of Field in struct Type.
#define offsetof(Type, Field) __builtin_offsetof(Type, Field)

#endif // SFOS_LIBC_STDDEF_H
