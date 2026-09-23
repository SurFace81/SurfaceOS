#include "../../include/cpu/tss.h"
#include "../../include/cpu/gdt.h"
#include "../../include/mm/memory.h"

static tss_t current_tss;

// Dedicated stacks for the faults that cannot trust the interrupted stack.
// 16 KiB each is plenty: nothing on these paths recurses, it only has to be
// enough to print a diagnostic.
#define IST_STACK_SIZE (16 * 1024)
static uint8_t df_stack[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t nmi_stack[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t mc_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

// Fill the 16-byte GDT system descriptor for a 64-bit TSS
static void install_gdt_entry(tss_t* tss)
{
    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(tss_t) - 1;

    // Low 8 bytes: limit[0:15] | base[0:23] | access(0x89: present, 64-bit TSS) | flags/limit[16:19]
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (uint64_t)((base & 0xFFFF) << 16);
    low |= (uint64_t)(((base >> 16) & 0xFF) << 32);
    low |= (uint64_t)0x89 << 40;                  // P=1, type=1001 (available 64-bit TSS)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48; // flags nibble stays 0
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    // High 8 bytes: base[32:63]
    uint64_t high = base >> 32;

    DefaultGDT.TssLow  = low;
    DefaultGDT.TssHigh = high;
}

namespace tss
{
    void init()
    {
        memory::memset((uint8_t*)&current_tss, 0x00, sizeof(current_tss));

        current_tss.ist1 = (uint64_t)df_stack  + IST_STACK_SIZE;
        current_tss.ist2 = (uint64_t)nmi_stack + IST_STACK_SIZE;
        current_tss.ist3 = (uint64_t)mc_stack  + IST_STACK_SIZE;

        // iomap_base past the segment limit means "no I/O permission bitmap",
        // so every I/O port access from ring 3 raises #GP.
        current_tss.iomap_base = sizeof(tss_t);

        install_gdt_entry(&current_tss);

        // Reload GDT (size changed) and load the task register
        gdt_ptr_t gdt_ptr;
        gdt_ptr.limit = sizeof(GDT_t) - 1;
        gdt_ptr.base = (uint64_t)&DefaultGDT;
        LoadGDT(&gdt_ptr);

        asm volatile("ltr %0" :: "r"((uint16_t)TSS_SEGMENT));
    }

    void set_kernel_stack(uint64_t rsp0)
    {
        current_tss.rsp0 = rsp0;
    }
} // namespace tss
