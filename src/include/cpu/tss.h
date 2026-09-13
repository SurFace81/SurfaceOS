#ifndef TSS_H
#define TSS_H

#include "types.h"

// 64-bit Task State Segment (only RSP0 and IST are used by this kernel)
struct tss_t
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define TSS_SEGMENT 0x30    // GDT slot of the TSS descriptor

namespace tss
{
    void init();

    // Kernel stack used by the CPU on ring3 -> ring0 transitions
    void set_kernel_stack(uint64_t rsp0);
}

#endif // TSS_H
