#include "../../include/cpu/cpuid.h"

namespace cpuid {
    static void cpuid(UINT32 function, UINT32* eax, UINT32* ebx, UINT32* ecx, UINT32* edx) {
        asm volatile(
            "cpuid"
            : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
            : "a"(function)
        );
    }

    void get_cpu_name(char* cpu_name) {
        UINT32 eax, ebx, ecx, edx;
    
        // Проверяем, поддерживаются ли расширенные функции CPUID
        cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x80000004) {
            // Если не поддерживается
            const char* unknown = "Unknown CPU";
            int i = 0;
            while (unknown[i] != '\0') {
                cpu_name[i] = unknown[i];
                i++;
            }
            cpu_name[i] = '\0';
            return;
        }
        
        // Получаем название процессора из трёх частей
        // 0x80000002, 0x80000003, 0x80000004 — каждая даёт 16 символов
        for (int part = 0; part < 3; part++) {
            cpuid(0x80000002 + part, &eax, &ebx, &ecx, &edx);
            
            // Записываем 16 байт из четырёх регистров
            *((UINT32*)&cpu_name[part * 16 + 0]) = eax;
            *((UINT32*)&cpu_name[part * 16 + 4]) = ebx;
            *((UINT32*)&cpu_name[part * 16 + 8]) = ecx;
            *((UINT32*)&cpu_name[part * 16 + 12]) = edx;
        }
        cpu_name[48] = '\0';
        
        // Убираем ведущие пробелы
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

    void get_cpu_frequency(UINT32* base_freq_mhz, UINT32* max_freq_mhz, UINT32* bus_freq_mhz) {
        UINT32 eax, ebx, ecx, edx;
        
        *base_freq_mhz = 0;
        *max_freq_mhz = 0;
        *bus_freq_mhz = 0;
        
        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x16) return;        
        cpuid(0x16, &eax, &ebx, &ecx, &edx);
        
        // EAX = базовая частота в МГц
        *base_freq_mhz = eax;        
        // EBX = максимальная частота в МГц
        *max_freq_mhz = ebx;        
        // ECX = частота шины
        *bus_freq_mhz = ecx;
    }

    UINT32 get_base_freq(void) {
        UINT32 base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return base_freq;
    }

    UINT32 get_max_freq(void) {
        UINT32 base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return max_freq;
    }

    UINT32 get_bus_freq(void) {
        UINT32 base_freq, max_freq, bus_freq;
        get_cpu_frequency(&base_freq, &max_freq, &bus_freq);
        return bus_freq;
    }

    void get_cpu_features(CPUFeatures* features) {
        UINT32 eax, ebx, ecx, edx;
        *features = {};
        
        cpuid(0x01, &eax, &ebx, &ecx, &edx);
        
        // Флаги из EDX (стандартные возможности)
        features->fpu       = (edx & (1 << 0)) != 0;    // Floating Point Unit
        features->pae       = (edx & (1 << 6)) != 0;    // Physical Address Extension
        features->apic      = (edx & (1 << 9)) != 0;    // APIC on chip
        features->mtrr      = (edx & (1 << 12)) != 0;   // Memory Type Range Registers
        features->pge       = (edx & (1 << 13)) != 0;   // Global Bit in Page Tables
        features->cmov      = (edx & (1 << 15)) != 0;   // Conditional Move
        features->pat       = (edx & (1 << 16)) != 0;   // Page Attribute Table
        features->clflush   = (edx & (1 << 19)) != 0;   // CLFLUSH instruction
        features->dts       = (edx & (1 << 21)) != 0;   // Debug Store
        features->acpi      = (edx & (1 << 22)) != 0;   // Thermal Monitor and Clock Control
        features->mmx       = (edx & (1 << 23)) != 0;   // MMX instructions
        features->fxsr      = (edx & (1 << 24)) != 0;   // FXSAVE/FXRSTOR
        features->sse       = (edx & (1 << 25)) != 0;   // SSE instructions
        features->sse2      = (edx & (1 << 26)) != 0;   // SSE2 instructions
        features->pbe       = (edx & (1 << 31)) != 0;   // Pending Break Enable
        
        // Флаги из ECX (дополнительные возможности)
        features->sse3      = (ecx & (1 << 0)) != 0;    // SSE3 instructions
        features->pclmulqdq = (ecx & (1 << 1)) != 0;    // PCLMULQDQ instruction
        features->monitor   = (ecx & (1 << 3)) != 0;    // MONITOR/MWAIT
        features->vmx       = (ecx & (1 << 5)) != 0;    // Virtual Machine Extensions
        features->smx       = (ecx & (1 << 6)) != 0;    // Safer Mode Extensions
        features->ssse3     = (ecx & (1 << 9)) != 0;    // Supplemental SSE3
        features->fma       = (ecx & (1 << 12)) != 0;   // Fused Multiply Add
        features->cx16      = (ecx & (1 << 13)) != 0;   // CMPXCHG16B
        features->pcid      = (ecx & (1 << 17)) != 0;   // Process Context Identifiers
        features->sse4_1    = (ecx & (1 << 19)) != 0;   // SSE4.1
        features->sse4_2    = (ecx & (1 << 20)) != 0;   // SSE4.2
        features->x2apic    = (ecx & (1 << 21)) != 0;   // Extended xAPIC
        features->movbe     = (ecx & (1 << 22)) != 0;   // MOVBE instruction
        features->popcnt    = (ecx & (1 << 23)) != 0;   // POPCNT instruction
        features->tsc_deadline = (ecx & (1 << 24)) != 0;// TSC-deadline APIC timer
        features->aes       = (ecx & (1 << 25)) != 0;   // AES instructions
        features->xsave     = (ecx & (1 << 26)) != 0;   // XSAVE/XRSTOR
        features->osxsave   = (ecx & (1 << 27)) != 0;   // XSAVE enabled by OS
        features->avx       = (ecx & (1 << 28)) != 0;   // AVX instructions
        features->rdrnd     = (ecx & (1 << 30)) != 0;   // RDRAND instruction
        
        // Расширенные флаги (leaf 0x07, subleaf 0)
        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x07) {
            cpuid(0x07, &eax, &ebx, &ecx, &edx);
            
            features->fsgsbase      = (ebx & (1 << 0)) != 0;    // FSGSBASE instructions
            features->tsc_adjust    = (ebx & (1 << 1)) != 0;    // TSC_ADJUST MSR
            features->bmi1          = (ebx & (1 << 3)) != 0;    // Bit Manipulation Instruction Set 1
            features->avx2          = (ebx & (1 << 5)) != 0;    // AVX2 instructions
            features->smep          = (ebx & (1 << 7)) != 0;    // Supervisor Mode Execution Prevention
            features->bmi2          = (ebx & (1 << 8)) != 0;    // Bit Manipulation Instruction Set 2
            features->erms          = (ebx & (1 << 9)) != 0;    // Enhanced REP MOVSB/STOSB
            features->invpcid       = (ebx & (1 << 10)) != 0;   // INVPCID instruction
            features->avx512f       = (ebx & (1 << 16)) != 0;   // AVX-512 Foundation
            features->avx512dq      = (ebx & (1 << 17)) != 0;   // AVX-512 Doubleword and Quadword
            features->rdseed        = (ebx & (1 << 18)) != 0;   // RDSEED instruction
            features->adx           = (ebx & (1 << 19)) != 0;   // Multi-Precision Add-Carry
            features->smap          = (ebx & (1 << 20)) != 0;   // Supervisor Mode Access Prevention
            features->avx512ifma    = (ebx & (1 << 21)) != 0;   // AVX-512 Integer Fused Multiply-Add
            features->clflushopt    = (ebx & (1 << 23)) != 0;   // CLFLUSHOPT instruction
            features->clwb          = (ebx & (1 << 24)) != 0;   // CLWB instruction
            features->sha           = (ebx & (1 << 29)) != 0;   // SHA-1 and SHA-256 instructions
            features->avx512bw      = (ebx & (1 << 30)) != 0;   // AVX-512 Byte and Word
            features->avx512vl      = (ebx & (1 << 31)) != 0;   // AVX-512 Vector Length Extensions
        }
        
        // AMD расширенные флаги (leaf 0x80000001)
        cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x80000001) {
            cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
            
            // EDX флаги для AMD
            features->syscall       = (edx & (1 << 11)) != 0;   // SYSCALL/SYSRET
            features->mp            = (edx & (1 << 19)) != 0;   // MP Capable
            features->nx            = (edx & (1 << 20)) != 0;   // No-Execute Page Protection
            features->rdtscp        = (edx & (1 << 27)) != 0;   // RDTSCP instruction
            features->lm            = (edx & (1 << 29)) != 0;   // Long Mode (64-bit)
            
            // ECX флаги для AMD
            features->lahf_lm       = (ecx & (1 << 0)) != 0;    // LAHF/SAHF in 64-bit mode
            features->abm           = (ecx & (1 << 5)) != 0;    // Advanced bit manipulation
            features->sse4a         = (ecx & (1 << 6)) != 0;    // SSE4a
            features->misalignsse   = (ecx & (1 << 7)) != 0;    // Misaligned SSE
            features->nodeid        = (ecx & (1 << 19)) != 0;   // NodeId MSR
            features->topoext       = (ecx & (1 << 22)) != 0;   // Topology Extensions
            features->perfctr_core  = (ecx & (1 << 23)) != 0;   // Core performance counter extensions
            features->perfctr_nb    = (ecx & (1 << 24)) != 0;   // NB performance counter extensions
            features->dbx           = (ecx & (1 << 26)) != 0;   // Data breakpoint extensions
        }
    }

    void get_cache_info(CacheInfo* cache) {
        UINT32 eax, ebx, ecx, edx;
        *cache = {};

        cpuid(0x00000000, &eax, &ebx, &ecx, &edx);
        if (eax < 0x04) return;

        for (UINT32 i = 0; i < 16; i++) {
            ecx = i;
            asm volatile(
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(0x04), "c"(i)
            );

            UINT32 cache_type = eax & 0x1F;
            if (cache_type == 0) break;

            UINT32 cache_level = (eax >> 5) & 0x7;
            UINT32 ways = ((ebx >> 22) & 0x3FF) + 1;
            UINT32 partitions = ((ebx >> 12) & 0x3FF) + 1;
            UINT32 line_size = (ebx & 0xFFF) + 1;
            UINT32 sets = ecx + 1;
            UINT32 size = (ways * partitions * line_size * sets) / 1024;

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
        UINT32 eax, ebx, ecx, edx;
        UINT32 threads_per_core = 1;
        UINT32 cores_per_package = 1;

        for (UINT32 level = 0; level < 4; ++level) {
            asm volatile(
                "cpuid"
                : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "a"(0x1F), "c"(level)
            );

            UINT32 level_type = (ecx >> 8) & 0xFF;
            if (level_type == 0) break;

            UINT32 processors_at_level = ebx & 0xFFFF;

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