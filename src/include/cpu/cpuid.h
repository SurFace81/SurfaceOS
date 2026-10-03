#ifndef CPUID_H
#define CPUID_H

#include "types.h"

// The caches of the core the caller runs on (CPUID leaf 4). L1 and L2
// belong to a core, L3 is shared.
struct CacheInfo {
    uint32_t l1d_size;      // L1 data cache in Kb
    uint32_t l1i_size;      // L1 instruction cache in Kb
    uint32_t l2_size;       // L2 cache in Kb
    uint32_t l3_size;       // L3 cache in Kb
    
    uint32_t l1d_ways;      // L1 data associativity
    uint32_t l1i_ways;      // L1 instruction associativity
    uint32_t l2_ways;       // L2 associativity
    uint32_t l3_ways;       // L3 associativity
    
    uint32_t l1d_linesize;  // L1 data line size
    uint32_t l1i_linesize;  // L1 instruction line size
    uint32_t l2_linesize;   // L2 line size
    uint32_t l3_linesize;   // L3 line size
};
#define CORE_EFFICIENCY  0x20   // Intel Atom
#define CORE_PERFORMANCE 0x40   // Intel Core

struct CPUTopology {
    uint32_t logical_cores;
    uint32_t physical_cores;
    uint32_t packages;         // num of sockets/cpus
    bool hyperthreading;
    bool hybrid;               // performance and efficiency cores
    uint32_t performance_cores;// hybrid: physical cores of each kind, among
    uint32_t efficiency_cores; // the CPUs that run (a core type is per CPU)
};

namespace cpuid {
    void get_cpu_name(char* cpu_name);
    uint32_t get_base_freq(void);
    uint32_t get_max_freq(void);
    uint32_t get_bus_freq(void);
    void get_cache_info(CacheInfo* cache);
    void get_cpu_topology(CPUTopology* topology);

    // The kind of core this runs on, on a hybrid CPU (CPUID leaf 0x1A):
    // CORE_PERFORMANCE, CORE_EFFICIENCY, or 0 when it does not say.
    uint8_t core_type();
}

#endif