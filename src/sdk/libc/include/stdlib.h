#ifndef SFOS_LIBC_STDLIB_H
#define SFOS_LIBC_STDLIB_H

#include <abi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Reads an integer from Text: spaces, a sign, then digits in Base (2..36,
/// or 0: "0x" means 16, a leading 0 means 8, else 10). *End, when End is
/// not null, gets where reading stopped - Text when there was no number.
/// Too large a number gives the largest value of the type.
long               strtol(const char* Text, char** End, int Base);
/// strtol(Text, NULL, 10) as an int.
int                atoi(const char* Text);
/// strtol for unsigned long; a '-' negates the result as unsigned.
unsigned long      strtoul(const char* Text, char** End, int Base);
/// strtol for unsigned long long; a '-' negates the result as unsigned.
unsigned long long strtoull(const char* Text, char** End, int Base);

/// Reads a decimal floating-point number from Text: spaces, a sign,
/// digits with an optional '.', an optional exponent (e, E). *End as
/// strtol's.
double      strtod(const char* Text, char** End);
/// strtod for float.
float       strtof(const char* Text, char** End);
/// strtod for long double.
long double strtold(const char* Text, char** End);

/// Sorts Count elements of Size bytes at Base, in the order of Compare
/// (<0, 0 or >0). Not stable.
void qsort(void* Base, size_t Count, size_t Size,
           int (*Compare)(const void* A, const void* B));

#ifdef __cplusplus
}
#endif

#endif // SFOS_LIBC_STDLIB_H
