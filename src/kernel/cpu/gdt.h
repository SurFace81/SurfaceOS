#ifndef GDT_H
#define GDT_H

#include "types.h"

// GDT pointers
struct gdt_ptr_struct {
    UINT16 limit;
    UINT64 base;
}__attribute__((packed));

typedef struct gdt_ptr_struct gdt_ptr_t;

// GDT entry
struct gdt_entry {
    UINT16  limit_low;
    UINT16  base_low;
    UINT8   base_middle;
    UINT8   access;
    UINT8   limit_flags;
    UINT8   base_high;
}__attribute__((packed));

typedef struct gdt_entry gdt_entry_t;

// GDT Table
struct GDT {
    gdt_entry_t Null;         // 0x00
    gdt_entry_t KernelCode;   // 0x08
    gdt_entry_t KernelData;   // 0x10
    gdt_entry_t UserNull;     // 0x18
    gdt_entry_t UserCode;     // 0x20
    gdt_entry_t UserData;     // 0x28
}__attribute__((packed))
__attribute__((aligned(0x1000)));

typedef struct GDT GDT_t;

// Functions
extern GDT_t DefaultGDT;
extern void LoadGDT(gdt_ptr_t* gdtDescriptor);
void initGDT();

#endif