#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include "types.h"

// Optional CPU protection/caching features. Everything here is probed with
// CPUID first: `-cpu qemu64` exposes far less than a real Skylake+ part, and
// executing e.g. STAC on a CPU without SMAP raises #UD.
namespace cpu
{
    // Enable, in this order: CR0.WP, EFER.NXE, CR4.SMEP, CR4.SMAP and a PAT
    // layout with a write-combining entry. Must run before any page table
    // uses PAGE_NX or PAGE_CACHE_WC.
    void init_features();

    // Print what actually got enabled. Separate call because init_features()
    // has to run before paging, i.e. long before the serial port exists.
    void log_features();

    bool has_nx();      // PAGE_NX is safe to set in a PTE
    bool has_smap();    // stac/clac are safe to execute
    bool has_pat();     // PAGE_CACHE_WC actually means write-combining

    // Set by init_features() once CR4.SMAP is on. Read directly by the
    // helpers below so they stay inlinable; stac/clac raise #UD when the
    // CPU has no SMAP, which `-cpu qemu64` does not.
    extern bool smap_enabled;

    // Allow supervisor access to user pages for the duration of a
    // copy_to_user/copy_from_user. No-ops unless SMAP is active.
    static inline void user_access_begin()
    {
        if (smap_enabled)
            asm volatile("stac" ::: "cc", "memory");
    }

    static inline void user_access_end()
    {
        if (smap_enabled)
            asm volatile("clac" ::: "cc", "memory");
    }
}

#endif // CPU_FEATURES_H
