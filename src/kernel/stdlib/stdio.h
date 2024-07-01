#ifndef STDIO_H
#define STDIO_H

#include "../cpu/types.h"

void printf(char*);
void printi(int);
void printh(UINT64, UINT64 size);
void clearScreen();
void setTextColor(UINT32);
void setCursorPosition(UINT32 x, UINT32 y);

#endif