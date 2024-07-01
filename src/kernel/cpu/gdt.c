#include "gdt.h"

__attribute__((aligned(0x1000)))
GDT_t DefaultGDT = {
    {0, 0, 0, 0x00, 0x00, 0}, // Null
    {0, 0, 0, 0x9A, 0xA0, 0}, // Kernel Code
    {0, 0, 0, 0x92, 0xA0, 0}, // Kernel Data
    {0, 0, 0, 0x00, 0x00, 0}, // Null
    {0, 0, 0, 0xFA, 0xA0, 0}, // User Code
    {0, 0, 0, 0xF2, 0xC0, 0}, // User Data
};

initGDT() {
    gdt_ptr_t gdtPtr;
    gdtPtr.limit = sizeof(GDT_t) - 1;
    gdtPtr.base = (UINT64)&DefaultGDT;

    LoadGDT(&gdtPtr);
}