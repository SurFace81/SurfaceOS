#ifndef PIT_H
#define PIT_H

#include "../cpu/types.h"
#include "../cpu/ports.h"
#include "../cpu/irq.h"

// PIT I/O ports
#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

// PIT base frequency (Hz)
#define PIT_BASE_FREQ   1193182

// Default tick rate (Hz) - 1000 Hz = 1ms per tick
#define PIT_DEFAULT_HZ  1000

namespace pit
{
    void init();
    void init(uint32_t frequency_hz);
    void handler();

    // System uptime in milliseconds
    uint64_t uptime_ms();

    // Current tick count since boot
    uint64_t ticks();

    // Blocking delay
    void sleep_ms(uint32_t ms);

    // Current configured frequency
    uint32_t frequency();
    void calibrate();
    uint32_t real_frequency();
}

#endif