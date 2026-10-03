#ifndef SFOS_LIBC_STDIO_H
#define SFOS_LIBC_STDIO_H

#include <abi/types.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Formatting only: printing and files are the SDK's (Sys->Console,
// Sys->Files).

/// Formats into Buffer, at most Size bytes with the NUL. Returns the
/// length the whole text has, without the NUL - Size or more when it was
/// cut short.
///
/// Conversions: d i u x X o c s p f %; flags - 0 + space #; width and
/// precision, also as *; sizes hh h l ll z j t, L for f.
int vsnprintf(char* Buffer, size_t Size, const char* Format, va_list Args);
/// vsnprintf with the arguments in place.
int snprintf(char* Buffer, size_t Size, const char* Format, ...);
/// snprintf into a Buffer that is large enough.
int sprintf(char* Buffer, const char* Format, ...);

#ifdef __cplusplus
}
#endif

#endif // SFOS_LIBC_STDIO_H
