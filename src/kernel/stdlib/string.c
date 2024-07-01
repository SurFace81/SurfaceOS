#include "string.h"
#include "../cpu/types.h"

const char nums_table[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void int_to_str(int num, char int_str[]) {
    UINT32 mod;
    int res = num;

    int i = 0;
    do {
        mod = res % 10;
        res = res / 10;

        int_str[i] = nums_table[mod];
        i++;
    } while(res >= 10);

    if (res != 0) {
        int_str[i] = nums_table[res];
        int_str[i + 1] = '\0';
    } else {
        int_str[i] = '\0';
    }
    reverse(int_str);
}

void hex_to_str(UINT64 num, char hex_str[], UINT64 size) {
    UINT64 mod, res = num;

    UINT64 i = size - 1;
    do {
        mod = res % 16;
        res = res / 16;

        hex_str[i] = nums_table[mod];
        i--;
    } while(res >= 16);

    hex_str[i] = nums_table[res];

    for (UINT64 j = 0; j < i; j++) {
        hex_str[j] = '0';
    }
    hex_str[size] = '\0';
}

void reverse(char s[]) {
    int c, i, j;
    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

int strlen(char s[]) {
    int i = 0;
    while (s[i] != '\0') ++i;
    return i;
}

void append(char s[], char n) {
    int len = strlen(s);
    s[len] = n;
    s[len+1] = '\0';
}

void backspace(char s[]) {
    int len = strlen(s);
    s[len-1] = '\0';
}

int strcmp(char s1[], char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}
