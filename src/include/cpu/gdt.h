#ifndef GDT_H
#define GDT_H

#include "types.h"

// GDT pointers
struct gdt_ptr_struct {
    uint16_t limit;
    uint64_t base;
}__attribute__((packed));

typedef struct gdt_ptr_struct gdt_ptr_t;

// GDT entry
struct gdt_entry {
    uint16_t  limit_low;
    uint16_t  base_low;
    uint8_t   base_middle;
    uint8_t   access;
    uint8_t   limit_flags;
    uint8_t   base_high;
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
extern "C" void LoadGDT(gdt_ptr_t* gdtDescriptor);

namespace gdt {
    void init();
}

#endif  // GDT_H