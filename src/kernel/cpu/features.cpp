// CPU protection and memory-type features.
//
// Everything is probed before it is enabled. The kernel has to run both on
// `-cpu qemu64` (no SMEP, no SMAP, no INVPCID) and on a modern laptop, and
// several of these instructions fault outright when the feature bit is clear.

#include "../../include/cpu/features.h"
#include "../../include/drivers/uart.h"

#define MSR_EFER            0xC0000080U
#define MSR_IA32_PAT        0x00000277U

#define EFER_NXE            (1ULL << 11)

#define CR0_MP              (1ULL << 1)
#define CR0_EM              (1ULL << 2)
#define CR0_TS              (1ULL << 3)
#define CR0_NE              (1ULL << 5)
#define CR0_WP              (1ULL << 16)
#define CR0_NW              (1ULL << 29)
#define CR0_CD              (1ULL << 30)

#define CR4_PGE             (1ULL << 7)
#define CR4_OSFXSR          (1ULL << 9)
#define CR4_OSXMMEXCPT      (1ULL << 10)
#define CR4_SMEP            (1ULL << 20)
#define CR4_SMAP            (1ULL << 21)

namespace
{
    inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                            uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
    {
        asm volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
    }

    inline uint64_t rdmsr(uint32_t msr)
    {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }

    inline void wrmsr(uint32_t msr, uint64_t value)
    {
        asm volatile("wrmsr"
                     :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
    }

    inline uint64_t read_cr0() { uint64_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
    inline void     write_cr0(uint64_t v) { asm volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
    inline uint64_t read_cr4() { uint64_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
    inline void     write_cr4(uint64_t v) { asm volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }
    inline uint64_t read_cr3() { uint64_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
    inline void     write_cr3(uint64_t v) { asm volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }

    bool nx_ok   = false;
    bool smep_ok = false;
    bool pat_ok  = false;
    bool pge_ok  = false;

    // Reprogram IA32_PAT so that index 7 (PAT=1, PCD=1, PWT=1) means
    // write-combining. Indices 0..6 keep their power-on meaning, so every
    // mapping that does not set the PAT bit behaves exactly as before.
    //
    //   PA0 WB(06)  PA1 WT(04)  PA2 UC-(07)  PA3 UC(00)
    //   PA4 WB(06)  PA5 WT(04)  PA6 UC-(07)  PA7 WC(01)
    //
    // Sequence follows Intel SDM Vol.3 "Programming the PAT": stop caching,
    // write back and invalidate, flush the TLB, swap the MSR, flush again.
    const uint64_t PAT_LAYOUT = 0x0107040600070406ULL;

    void program_pat()
    {
        uint64_t flags;
        asm volatile("pushfq; pop %0" : "=r"(flags));
        asm volatile("cli");

        uint64_t cr0 = read_cr0();
        write_cr0((cr0 & ~CR0_NW) | CR0_CD);    // CD=1, NW=0
        asm volatile("wbinvd" ::: "memory");

        uint64_t cr3 = read_cr3();
        write_cr3(cr3);                          // flush TLB

        wrmsr(MSR_IA32_PAT, PAT_LAYOUT);

        write_cr3(cr3);                          // flush TLB again
        asm volatile("wbinvd" ::: "memory");
        write_cr0(cr0);                          // restore caching

        if (flags & 0x200)
            asm volatile("sti");
    }
}

namespace cpu
{
    // Read by the inline helpers in features.h. Must stay false until SMAP
    // is actually turned on, otherwise stac/clac raise #UD.
    bool smap_enabled = false;

    void init_features()
    {
        uint32_t a, b, c, d;

        // CR0.WP: without it the kernel silently writes through read-only
        // PTEs, which would defeat the read-only mapping of app .text.
        write_cr0(read_cr0() | CR0_WP);

        // x87/SSE on, with FXSAVE/FXRSTOR usable. x86-64 guarantees SSE2, and
        // UEFI firmware normally leaves it enabled - but "normally" is not
        // something to build on: the scheduler saves every process's FPU
        // state with FXSAVE, and user code compiled by GCC uses XMM registers
        // freely. Set the bits explicitly instead of inheriting them.
        //   CR0: EM=0 (no emulation), MP=1, NE=1 (native #MF), TS=0
        //   CR4: OSFXSR (FXSAVE + SSE), OSXMMEXCPT (#XM instead of #UD)
        write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
        write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

        // NX (CPUID.80000001H:EDX[20]) -> EFER.NXE
        cpuid_count(0x80000000, 0, &a, &b, &c, &d);
        if (a >= 0x80000001)
        {
            cpuid_count(0x80000001, 0, &a, &b, &c, &d);
            if (d & (1U << 20))
            {
                wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
                nx_ok = true;
            }
        }

        // PAT (CPUID.01H:EDX[16])
        cpuid_count(1, 0, &a, &b, &c, &d);
        if (d & (1U << 16))
        {
            program_pat();
            pat_ok = true;
        }

        // Global pages (CPUID.01H:EDX[13]). The kernel half is marked
        // PAGE_GLOBAL, so its TLB entries survive every process switch.
        // Only the final tables carry the bit; kentry's boot tables do not,
        // so nothing global is cached before paging::init() replaces them.
        if (d & (1U << 13))
        {
            write_cr4(read_cr4() | CR4_PGE);
            pge_ok = true;
        }

        // SMEP (CPUID.07H:0:EBX[7]) and SMAP (bit 20)
        cpuid_count(0, 0, &a, &b, &c, &d);
        if (a >= 7)
        {
            cpuid_count(7, 0, &a, &b, &c, &d);
            uint64_t cr4 = read_cr4();

            if (b & (1U << 7))
            {
                cr4 |= CR4_SMEP;        // ring 0 may not execute user pages
                smep_ok = true;
            }
            if (b & (1U << 20))
            {
                cr4 |= CR4_SMAP;        // ring 0 may not touch user pages...
                write_cr4(cr4);
                smap_enabled = true;    // ...outside stac/clac (see uaccess.cpp)
            }
            else
            {
                write_cr4(cr4);
            }
        }

    }

    // Separate from init_features(): the features have to be on before the
    // first page table is built, which is long before the serial port works.
    void log_features()
    {
        uart::printf("cpu: nx=%u smep=%u smap=%u pat=%u pge=%u\n",
                     (uint32_t)nx_ok, (uint32_t)smep_ok,
                     (uint32_t)smap_enabled, (uint32_t)pat_ok, (uint32_t)pge_ok);
    }

    bool has_nx()   { return nx_ok; }
    bool has_smap() { return smap_enabled; }
    bool has_pat()  { return pat_ok; }
}
