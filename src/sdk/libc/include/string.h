#ifndef SFOS_LIBC_STRING_H
#define SFOS_LIBC_STRING_H

#include <abi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Copies Size bytes from Src to Dest; they must not overlap. Returns Dest.
void* memcpy(void* Dest, const void* Src, size_t Size);
/// Copies Size bytes from Src to Dest, which may overlap. Returns Dest.
void* memmove(void* Dest, const void* Src, size_t Size);
/// Fills Size bytes at Dest with (unsigned char)Value. Returns Dest.
void* memset(void* Dest, int Value, size_t Size);
/// Compares Size bytes as unsigned chars: <0, 0 or >0.
int   memcmp(const void* A, const void* B, size_t Size);

/// The length of Text, without its NUL.
size_t strlen(const char* Text);
/// Compares two strings as unsigned chars: <0, 0 or >0.
int    strcmp(const char* A, const char* B);
/// strcmp of at most Count characters.
int    strncmp(const char* A, const char* B, size_t Count);
/// Copies Src with its NUL to Dest. Returns Dest.
char*  strcpy(char* Dest, const char* Src);
/// Appends Src with its NUL to the string in Dest. Returns Dest.
char*  strcat(char* Dest, const char* Src);
/// The first Char in Text (the NUL counts), or NULL.
char*  strchr(const char* Text, int Char);
/// The last Char in Text (the NUL counts), or NULL.
char*  strrchr(const char* Text, int Char);
/// The first place Part occurs in Text, or NULL; Text itself for an empty Part.
char*  strstr(const char* Text, const char* Part);

#ifdef __cplusplus
}
#endif

#endif // SFOS_LIBC_STRING_H
