// Local APIC and IOAPIC. See apic.h.

#include "../../include/cpu/apic.h"
#include "../../include/cpu/irq.h"
#include "../../include/cpu/idt.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/percpu.h"
#include "../../include/acpi/acpi.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/pit.h"

namespace
{
    // Local APIC registers (byte offsets in xAPIC mode; MSR 0x800 + off/16
    // in x2APIC mode).
    const uint32_t LAPIC_ID      = 0x020;
    const uint32_t LAPIC_TPR     = 0x080;
    const uint32_t LAPIC_EOI     = 0x0B0;
    const uint32_t LAPIC_SVR     = 0x0F0;
    const uint32_t LAPIC_LVT_TIMER = 0x320;
    const uint32_t LAPIC_LVT_LINT0 = 0x350;
    const uint32_t LAPIC_LVT_ERROR = 0x370;
    const uint32_t LAPIC_ICR_LOW    = 0x300;
    const uint32_t LAPIC_ICR_HIGH   = 0x310;
    const uint32_t ICR_PENDING      = 1U << 12;
    const uint32_t ICR_INIT         = 0x4500;   // INIT, level assert
    const uint32_t ICR_STARTUP      = 0x4600;   // startup IPI; low byte: page number
    const uint32_t LAPIC_TIMER_INIT = 0x380;
    const uint32_t LAPIC_TIMER_CUR  = 0x390;
    const uint32_t LAPIC_TIMER_DIV  = 0x3E0;
    const uint32_t TIMER_DIV_16     = 0x3;
    const uint32_t LVT_PERIODIC     = 1U << 17;

    const uint32_t SVR_ENABLE    = 1U << 8;
    const uint32_t LVT_MASKED    = 1U << 16;

    const uint32_t MSR_APIC_BASE = 0x1B;
    const uint64_t APIC_BASE_EXTD   = 1ULL << 10;   // x2APIC mode
    const uint64_t APIC_BASE_ENABLE = 1ULL << 11;
    const uint32_t MSR_X2APIC    = 0x800;

    // IOAPIC: an index register and a data window.
    const uint32_t IOREGSEL      = 0x00;
    const uint32_t IOWIN         = 0x10;
    const uint32_t IOAPIC_VER    = 0x01;
    const uint32_t IOAPIC_REDTBL = 0x10;            // 2 registers per entry

    const uint64_t RED_ACTIVE_LOW = 1ULL << 13;
    const uint64_t RED_LEVEL      = 1ULL << 15;
    const uint64_t RED_MASKED     = 1ULL << 16;

    struct ioapic
    {
        volatile uint32_t* regs;
        uint32_t gsi_base;
        uint32_t entries;
    };

    bool      on = false;
    bool      x2apic = false;
    volatile uint32_t* lapic = nullptr;
    ioapic    ioapics[acpi::MAX_IOAPICS];
    uint32_t  ioapic_count = 0;
    uint32_t  bsp_id = 0;
    uint32_t  timer_count = 0;          // local APIC timer counts per tick

    inline uint64_t rdmsr(uint32_t msr)
    {
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }

    inline void wrmsr(uint32_t msr, uint64_t v)
    {
        asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    }

    uint32_t lapic_read(uint32_t reg)
    {
        if (x2apic)
            return (uint32_t)rdmsr(MSR_X2APIC + reg / 16);
        return lapic[reg / 4];
    }

    void lapic_write(uint32_t reg, uint32_t v)
    {
        if (x2apic)
            wrmsr(MSR_X2APIC + reg / 16, v);
        else
            lapic[reg / 4] = v;
    }

    uint32_t io_read(const ioapic& io, uint32_t reg)
    {
        io.regs[IOREGSEL / 4] = reg;
        return io.regs[IOWIN / 4];
    }

    void io_write(const ioapic& io, uint32_t reg, uint32_t v)
    {
        io.regs[IOREGSEL / 4] = reg;
        io.regs[IOWIN / 4] = v;
    }

    void write_entry(const ioapic& io, uint32_t index, uint64_t entry)
    {
        // High half (destination) first, so the entry is never live with a
        // stale destination.
        io_write(io, IOAPIC_REDTBL + index * 2 + 1, (uint32_t)(entry >> 32));
        io_write(io, IOAPIC_REDTBL + index * 2, (uint32_t)entry);
    }

    // Turn this CPU's local APIC on, everything but the spurious vector
    // masked.
    void enable_local()
    {
        lapic_write(LAPIC_TPR, 0);
        lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
        lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);   // the 8259's line: not used
        lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
        lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    }

    uint32_t local_id()
    {
        return x2apic ? lapic_read(LAPIC_ID) : lapic_read(LAPIC_ID) >> 24;
    }

    void send_ipi(uint32_t apic_id, uint32_t low)
    {
        if (x2apic)
        {
            wrmsr(MSR_X2APIC + LAPIC_ICR_LOW / 16, ((uint64_t)apic_id << 32) | low);
            return;
        }
        lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, low);
        while (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING)
            asm volatile("pause");
    }

    // The GSI, polarity and trigger mode of ISA IRQ `irq`.
    void isa_route(uint8_t irq, uint32_t* gsi, bool* active_low, bool* level)
    {
        *gsi = irq;
        *active_low = false;                // ISA: active high,
        *level = false;                     // edge-triggered
        const acpi::madt_info* m = acpi::madt();
        for (uint32_t i = 0; i < m->override_count; i++)
            if (m->overrides[i].irq == irq)
            {
                *gsi        = m->overrides[i].gsi;
                *active_low = m->overrides[i].active_low;
                *level      = m->overrides[i].level;
            }
    }

    // The IOAPIC serving `gsi` and its entry there, or nullptr.
    const ioapic* ioapic_for(uint32_t gsi, uint32_t* index)
    {
        for (uint32_t i = 0; i < ioapic_count; i++)
            if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].entries)
            {
                *index = gsi - ioapics[i].gsi_base;
                return &ioapics[i];
            }
        return nullptr;
    }

    // Program ISA IRQ `irq`: vector IRQ_BASE + irq, to this CPU.
    void route(uint8_t irq, bool masked)
    {
        uint32_t gsi, index;
        bool active_low, level;
        isa_route(irq, &gsi, &active_low, &level);
        const ioapic* io = ioapic_for(gsi, &index);
        if (!io)
            return;
        uint64_t entry = (uint64_t)(IRQ_BASE + irq) | ((uint64_t)bsp_id << 56);
        if (active_low)
            entry |= RED_ACTIVE_LOW;
        if (level)
            entry |= RED_LEVEL;
        if (masked)
            entry |= RED_MASKED;
        write_entry(*io, index, entry);
    }
}

namespace apic
{
    bool init()
    {
        const acpi::madt_info* m = acpi::madt();
        if (!m->present || !m->lapic_address || m->ioapic_count == 0)
        {
            uart::printf("apic: no MADT or IOAPIC, staying on the 8259s\n");
            return false;
        }

        uint64_t base = rdmsr(MSR_APIC_BASE);
        x2apic = (base & APIC_BASE_EXTD) != 0;
        if (!(base & APIC_BASE_ENABLE))
            wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
        if (!x2apic)
            lapic = (volatile uint32_t*)paging::map_mmio_region(m->lapic_address, 0x1000);

        for (uint32_t i = 0; i < m->ioapic_count; i++)
        {
            ioapic& io = ioapics[ioapic_count++];
            io.regs     = (volatile uint32_t*)paging::map_mmio_region(m->ioapics[i].address, 0x1000);
            io.gsi_base = m->ioapics[i].gsi_base;
            io.entries  = ((io_read(io, IOAPIC_VER) >> 16) & 0xFF) + 1;
            for (uint32_t e = 0; e < io.entries; e++)
                write_entry(io, e, RED_MASKED);
        }

        // The 8259s stay remapped (a stray interrupt from them lands on a
        // harmless vector) and fully masked.
        irq::mask_all();

        idt::set_entry(APIC_SPURIOUS_VECTOR, (uint64_t)apic_spurious, IDT_FLAG_INTERRUPT_GATE);
        enable_local();

        bsp_id = local_id();
        cpu::current()->apic_id = bsp_id;
        for (uint8_t irq = 0; irq < 16; irq++)
            route(irq, true);
        on = true;

        uart::printf("apic: %s mode, CPU APIC id %u, %u IOAPIC(s), %u entries on the first\n",
                     x2apic ? "x2APIC" : "xAPIC", bsp_id, ioapic_count, ioapics[0].entries);
        return true;
    }

    bool active()
    {
        return on;
    }

    void eoi()
    {
        lapic_write(LAPIC_EOI, 0);
    }

    void unmask_irq(uint8_t irq)
    {
        route(irq, false);
    }

    void mask_irq(uint8_t irq)
    {
        route(irq, true);
    }

    uint32_t id()
    {
        return bsp_id;
    }

    void start_timer_cpu()
    {
        lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
        lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | (IRQ_BASE + IRQ_APIC_TIMER));
        lapic_write(LAPIC_TIMER_INIT, timer_count);
    }

    uint32_t init_cpu()
    {
        // Same mode as the boot CPU's (xAPIC to x2APIC is a legal switch;
        // the registers of an xAPIC are the same page on every CPU).
        uint64_t base = rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE;
        if (x2apic)
            base |= APIC_BASE_EXTD;
        wrmsr(MSR_APIC_BASE, base);
        enable_local();
        return local_id();
    }

    void send_init(uint32_t apic_id)
    {
        send_ipi(apic_id, ICR_INIT);
    }

    void send_startup(uint32_t apic_id, uint64_t page)
    {
        send_ipi(apic_id, ICR_STARTUP | (uint32_t)(page >> 12));
    }

    bool start_timer()
    {
        if (!on)
            return false;
        idt::set_entry(IRQ_BASE + IRQ_APIC_TIMER, (uint64_t)irq16, IDT_FLAG_INTERRUPT_GATE);
        lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
        lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);

        // Count down from the top across CALIBRATION PIT ticks, starting
        // on a tick edge.
        const uint32_t CALIBRATION = 100;
        uint64_t t = pit::ticks();
        while (pit::ticks() == t)
            asm volatile("pause");
        lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
        t = pit::ticks();
        while (pit::ticks() - t < CALIBRATION)
            asm volatile("pause");
        uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
        lapic_write(LAPIC_TIMER_INIT, 0);

        uint32_t per_tick = elapsed / CALIBRATION;
        if (!per_tick)
        {
            uart::printf("apic: the local APIC timer does not count, staying on the PIT\n");
            return false;
        }

        // The PIT falls silent and the local APIC ticks in its place.
        mask_irq(IRQ0_TIMER);
        timer_count = per_tick;
        start_timer_cpu();

        // ARAT: the timer keeps running in deep C-states. Without it a CPU
        // idling in one could miss ticks; hlt (all the kernel uses) is C1.
        uint32_t eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(6), "c"(0));
        uart::printf("apic: local APIC timer drives the tick, %u counts per tick%s\n",
                     per_tick, (eax & (1U << 2)) ? "" : " (no ARAT)");
        return true;
    }
}
