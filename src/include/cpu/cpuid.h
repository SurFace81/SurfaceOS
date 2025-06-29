#ifndef CPUID_H
#define CPUID_H

#include "types.h"

struct CPUFeatures {
    // Основные флаги из leaf 0x01 (EDX)
    bool fpu, pae;
    bool apic, mtrr, pge, cmov, pat;
    bool clflush, dts, acpi, mmx, fxsr, sse;
    bool sse2, pbe;
    
    // Дополнительные флаги из leaf 0x01 (ECX)
    bool sse3, pclmulqdq, monitor, vmx, smx;
    bool ssse3, fma, cx16, pcid;
    bool sse4_1, sse4_2, x2apic, movbe, popcnt, tsc_deadline, aes;
    bool xsave, osxsave, avx, rdrnd;
    
    // Расширенные флаги из leaf 0x07 (EBX)
    bool fsgsbase, tsc_adjust, bmi1, avx2, fdp_excptn_only, smep;
    bool bmi2, erms, invpcid, pqm, fpcsds, pqe;
    bool avx512f, avx512dq, rdseed, adx, smap, avx512ifma, pcommit, clflushopt;
    bool clwb, sha, avx512bw, avx512vl;
    
    // AMD расширенные флаги из leaf 0x80000001 (EDX)
    bool syscall, mp, nx, rdtscp, lm;
    
    // AMD расширенные флаги из leaf 0x80000001 (ECX)
    bool lahf_lm,abm, sse4a, misalignsse;
    bool nodeid, topoext, perfctr_core, perfctr_nb, dbx;
};
struct CacheInfo {
    UINT32 l1d_size;      // L1 data cache в KB
    UINT32 l1i_size;      // L1 instruction cache в KB
    UINT32 l2_size;       // L2 cache в KB
    UINT32 l3_size;       // L3 cache в KB
    
    UINT32 l1d_ways;      // L1 data associativity
    UINT32 l1i_ways;      // L1 instruction associativity
    UINT32 l2_ways;       // L2 associativity
    UINT32 l3_ways;       // L3 associativity
    
    UINT32 l1d_linesize;  // L1 data line size
    UINT32 l1i_linesize;  // L1 instruction line size
    UINT32 l2_linesize;   // L2 line size
    UINT32 l3_linesize;   // L3 line size
};
struct CPUTopology {
    UINT32 logical_cores;
    UINT32 physical_cores;
    UINT32 packages;         // Количество сокетов/процессоров
    bool hyperthreading;
};

namespace cpuid {
    void get_cpu_name(char* cpu_name);
    UINT32 get_base_freq(void);
    UINT32 get_max_freq(void);
    UINT32 get_bus_freq(void);
    void get_cpu_features(CPUFeatures* features);
    void get_cache_info(CacheInfo* cache);
    void get_cpu_topology(CPUTopology* topology);
}

#endif