#include "../../include/cpu/cpuid.h"

namespace cpuid {
    static void cpuid(uint32_t function, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
        asm volatile(
            "cpuid"
            : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
            : "a"(function)
        );
    }

    void get_cpu_name(char* cpu_name) {
        uint32_t eax, ebx, ecx, edx;
    
        // Checks whether extended CPUID functions are supported
        cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x80000004) {
            // If not supported
            const char* unknown = "Unknown CPU";
            int i = 0;
            while (unknown[i] != '\0') {
                cpu_name[i] = unknown[i];
                i++;
            }
            cpu_name[i] = '\0';
            return;
        }
        
        // Get CPU name
        // 0x80000002, 0x80000003, 0x80000004 — each returns 16 chars
        for (int part = 0; part < 3; part++) {
            cpuid(0x80000002 + part, &eax, &ebx, &ecx, &edx);
            
            // Write 16 bytes from 4 regs
            *((uint32_t*)&cpu_name[part * 16 + 0]) = eax;
            *((uint32_t*)&cpu_name[part * 16 + 4]) = ebx;
            *((uint32_t*)&cpu_name[part * 16 + 8]) = ecx;
            *((uint32_t*)&cpu_name[part * 16 + 12]) = edx;
        }
        cpu_name[48] = '\0';
        
        char* start = cpu_name;
        while (*start == ' ') {
            start++;
        }

        if (start != cpu_name) {
            int i = 0;
            while (start[i] != '\0') {
                cpu_name[i] = start[i];
                i++;
            }
            cpu_name[i] = '\0';
        }
    }

    void get_cpu_frequency(uint32_t* base_freq_mhz, uint32_t* max_freq_mhz, uint32_t* bus_freq_mhz) {
        uint32_t eax, ebx, ecx, edx;
        
        *base_freq_mhz = 0;
        *max_freq_mhz = 0;
        *bus_freq_mhz = 0;
        
        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x16) return;        
        cpuid(0x16, &eax, &ebx, &ecx, &edx);
        
        // EAX = base freq in MHz
        *base_freq_mhz = eax;        
        // EBX = max freq in MHz
        *max_freq_mhz = ebx;        
        // ECX = bus freq
        *bus_freq_mhz = ecx;
    }

    uint32_t get_base_freq(void) {
        uint32_t base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return base_freq;
    }

    uint32_t get_max_freq(void) {
        uint32_t base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return max_freq;
    }

    uint32_t get_bus_freq(void) {
        uint32_t base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return bus_freq;
    }

    void get_cache_info(CacheInfo* cache) {
        uint32_t eax, ebx, ecx, edx;
        *cache = {};

        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x04) return;

        for (uint32_t i = 0; i < 16; i++) {
            ecx = i;
            asm volatile(
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(0x04), "c"(i)
            );

            uint32_t cache_type = eax & 0x1F;
            if (cache_type == 0) break;

            uint32_t cache_level = (eax >> 5) & 0x7;
            uint32_t ways = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t partitions = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t line_size = (ebx & 0xFFF) + 1;
            uint32_t sets = ecx + 1;
            uint32_t size = (ways * partitions * line_size * sets) / 1024;

            if (cache_level == 1) {
                if (cache_type == 1) {
                    cache->l1d_size = size;
                    cache->l1d_ways = ways;
                    cache->l1d_linesize = line_size;
                } else if (cache_type == 2) {
                    cache->l1i_size = size;
                    cache->l1i_ways = ways;
                    cache->l1i_linesize = line_size;
                }
            } else if (cache_level == 2) {
                cache->l2_size = size;
                cache->l2_ways = ways;
                cache->l2_linesize = line_size;
            } else if (cache_level == 3) {
                cache->l3_size = size;
                cache->l3_ways = ways;
                cache->l3_linesize = line_size;
            }
        }
    }
    
    void get_cpu_topology(CPUTopology* topology) {
        uint32_t eax, ebx, ecx, edx;
        uint32_t threads_per_core = 1;
        uint32_t cores_per_package = 1;
        
        for (uint32_t level = 0; level < 4; ++level) {
            asm volatile(
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(0x1F), "c"(level)
            );

            uint32_t level_type = (ecx >> 8) & 0xFF;
            if (level_type == 0) break;

            uint32_t processors_at_level = ebx & 0xFFFF;

            if (level_type == 1) {
                threads_per_core = processors_at_level;
            } else if (level_type == 2) {
                cores_per_package = processors_at_level;
            }
        }

        topology->logical_cores = cores_per_package;
        topology->physical_cores = cores_per_package / threads_per_core;
        topology->packages = 1;
        topology->hyperthreading = topology->logical_cores > topology->physical_cores;
    }
}