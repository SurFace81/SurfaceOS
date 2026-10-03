// libc: the str* functions of string.h.

#include <string.h>

size_t strlen(const char* Text)
{
    const char* End = Text;
    while (*End)
        End++;
    return (size_t)(End - Text);
}

int strcmp(const char* A, const char* B)
{
    while (*A && *A == *B)
    {
        A++;
        B++;
    }
    return (unsigned char)*A - (unsigned char)*B;
}

int strncmp(const char* A, const char* B, size_t Count)
{
    for (; Count; Count--, A++, B++)
        if (*A != *B || !*A)
            return (unsigned char)*A - (unsigned char)*B;
    return 0;
}

char* strcpy(char* Dest, const char* Src)
{
    char* D = Dest;
    while ((*D++ = *Src++))
        ;
    return Dest;
}

char* strcat(char* Dest, const char* Src)
{
    strcpy(Dest + strlen(Dest), Src);
    return Dest;
}

char* strchr(const char* Text, int Char)
{
    for (;; Text++)
    {
        if (*Text == (char)Char)
            return (char*)Text;
        if (!*Text)
            return NULL;
    }
}

char* strrchr(const char* Text, int Char)
{
    const char* Last = NULL;
    for (;; Text++)
    {
        if (*Text == (char)Char)
            Last = Text;
        if (!*Text)
            return (char*)Last;
    }
}

char* strstr(const char* Text, const char* Part)
{
    size_t Length = strlen(Part);
    for (; *Text || !Length; Text++)
        if (!strncmp(Text, Part, Length))
            return (char*)Text;
    return NULL;
}
