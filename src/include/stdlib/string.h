#ifndef STRING_H
#define STRING_H

#include "../cpu/types.h"

void int_to_str(int num, char str[]);
void hex_to_str(uint64_t num, char str[], uint64_t size);

int  strlen(const char* s);
int  strcmp(const char* s1, const char* s2);

#endif // STRING_H