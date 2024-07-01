#ifndef STRING_H
#define STRING_H

#include "efi.h"

#define NULL (void*)0

int strlen(CHAR16 s[]) {
    int i = 0;
    while (s[i] != '\0') ++i;
    return i;
}

void reverse(CHAR16 s[]) {
    int c, i, j;
    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}


const char nums_table[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void int_to_str(int num, CHAR16 int_str[]) {
    UINT32 mod;
    int res = num;

    int i = 0;
    do {
        mod = res % 10;
        res = res / 10;

        int_str[i] = nums_table[mod];
        i++;
    } while (res >= 10);

    int_str[i] = nums_table[res];
    int_str[i+1] = '\0';

    reverse(int_str);
}

void hex_to_str(UINT32 num, CHAR16 hex_str[], UINT32 size) {
    UINT32 mod, res = num;

    UINT32 i = size;
    do {
        mod = res % 16;
        res = res / 16;

        hex_str[i] = nums_table[mod];
        i--;
    } while (res > 16);

    hex_str[i] = nums_table[res];

    for (int j = 0; j < i; j++) {
        hex_str[j] = '0';
    }
    hex_str[size+1] = '\0';
}

int strcmp(char s1[], char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

#endif