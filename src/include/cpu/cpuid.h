#ifndef CPUID_H
#define CPUID_H

#include "types.h"

struct CacheInfo {
    uint32_t l1d_size;      // L1 data cache в KB
    uint32_t l1i_size;      // L1 instruction cache в KB
    uint32_t l2_size;       // L2 cache в KB
    uint32_t l3_size;       // L3 cache в KB
    
    uint32_t l1d_ways;      // L1 data associativity
    uint32_t l1i_ways;      // L1 instruction associativity
    uint32_t l2_ways;       // L2 associativity
    uint32_t l3_ways;       // L3 associativity
    
    uint32_t l1d_linesize;  // L1 data line size
    uint32_t l1i_linesize;  // L1 instruction line size
    uint32_t l2_linesize;   // L2 line size
    uint32_t l3_linesize;   // L3 line size
};
struct CPUTopology {
    uint32_t logical_cores;
    uint32_t physical_cores;
    uint32_t packages;         // Количество сокетов/процессоров
    bool hyperthreading;
};

namespace cpuid {
    void get_cpu_name(char* cpu_name);
    uint32_t get_base_freq(void);
    uint32_t get_max_freq(void);
    uint32_t get_bus_freq(void);
    void get_cache_info(CacheInfo* cache);
    void get_cpu_topology(CPUTopology* topology);
}

#endif