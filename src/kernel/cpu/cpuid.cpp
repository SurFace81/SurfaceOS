#include "../../include/cpu/cpuid.h"
#include "../../include/acpi/acpi.h"

namespace cpuid {
    static void cpuid(uint32_t function, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
        asm volatile(
            "cpuid"
            : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
            : "a"(function)
        );
    }

    static void cpuid_sub(uint32_t leaf, uint32_t sub, uint32_t* eax, uint32_t* ebx,
                          uint32_t* ecx, uint32_t* edx) {
        asm volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(sub));
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
        
        *base_freq_mhz = eax;
        *max_freq_mhz = ebx;
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

        // Intel describes its caches in leaf 4, AMD in 0x8000001D - the
        // same layout.
        uint32_t leaf = 0;
        cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x8000001D) {
            cpuid_sub(0x8000001D, 0, &eax, &ebx, &ecx, &edx);
            if (eax & 0x1F)
                leaf = 0x8000001D;
        }
        if (!leaf) {
            cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
            if (eax < 0x04) return;
            leaf = 0x04;
        }

        for (uint32_t i = 0; i < 16; i++) {
            cpuid_sub(leaf, i, &eax, &ebx, &ecx, &edx);

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
    
    // Counted from the CPUs the MADT lists: an APIC id holds the thread,
    // core and package numbers as bit fields, and CPUID leaf 0x1F (or 0xB)
    // tells their widths. Counting distinct core numbers is right on a
    // hybrid CPU too, where some cores run two threads and some one.
    void get_cpu_topology(CPUTopology* topology) {
        uint32_t eax, ebx, ecx, edx;
        uint32_t smt_shift = 0;         // APIC id >> smt_shift: the core
        uint32_t pkg_shift = 8;         // APIC id >> pkg_shift: the package

        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        uint32_t max_leaf = eax;
        uint32_t leaf = max_leaf >= 0x1F ? 0x1F : (max_leaf >= 0x0B ? 0x0B : 0);
        if (leaf) {
            for (uint32_t level = 0; level < 8; level++) {
                cpuid_sub(leaf, level, &eax, &ebx, &ecx, &edx);
                uint32_t type = (ecx >> 8) & 0xFF;
                if (type == 0)
                    break;
                if (type == 1)          // SMT: the bits below the core
                    smt_shift = eax & 0x1F;
                pkg_shift = eax & 0x1F; // the last level's shift is the package's
            }
        }

        const acpi::madt_info* m = acpi::madt();
        uint32_t cores[acpi::MAX_CPUS], packages[acpi::MAX_CPUS];
        uint32_t ncores = 0, npackages = 0, logical = 0;
        for (uint32_t i = 0; m->present && i < m->cpu_count; i++) {
            if (!m->cpus[i].enabled)
                continue;
            logical++;
            uint32_t core = m->cpus[i].apic_id >> smt_shift;
            uint32_t pkg  = m->cpus[i].apic_id >> pkg_shift;
            bool seen = false;
            for (uint32_t j = 0; j < ncores; j++)
                seen = seen || cores[j] == core;
            if (!seen)
                cores[ncores++] = core;
            seen = false;
            for (uint32_t j = 0; j < npackages; j++)
                seen = seen || packages[j] == pkg;
            if (!seen)
                packages[npackages++] = pkg;
        }
        if (!logical) {                 // no MADT: just this CPU
            logical = ncores = npackages = 1;
        }

        topology->logical_cores  = logical;
        topology->physical_cores = ncores;
        topology->packages       = npackages;
        topology->hyperthreading = logical > ncores;

        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        topology->hybrid = false;
        if (max_leaf >= 7) {
            cpuid_sub(7, 0, &eax, &ebx, &ecx, &edx);
            topology->hybrid = (edx >> 15) & 1;
        }
    }
}
