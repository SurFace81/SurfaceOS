#ifndef STRINGS_H
#define STRINGS_H

#include "../cpu/types.h"

void int_to_str(int num, char int_str[]);
void hex_to_str(UINT64 num, char hex_str[], UINT64 size);
void reverse(char s[]);
int strlen(char s[]);
void backspace(char s[]);
void append(char s[], char n);
int strcmp(char s1[], char s2[]);

#endif
