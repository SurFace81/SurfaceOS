#include "../../include/drivers/pit.h"
#include "../../include/drivers/screen.h"
#include "../../include/drivers/rtc.h"

static volatile uint64_t tick_count = 0;
static uint32_t pit_freq = PIT_DEFAULT_HZ;
static uint32_t ms_per_tick = 1;      // milliseconds per tick (integer part)
static uint32_t tick_remainder = 0;   // accumulator for sub-ms precision
static uint32_t remainder_step = 0;   // fractional part per tick (* 1000)
static volatile uint64_t uptime = 0;  // accumulated milliseconds

namespace pit
{
    static uint32_t real_freq = PIT_DEFAULT_HZ;

    void calibrate()
    {
        uint8_t start_sec = rtc::seconds();
        while (rtc::seconds() == start_sec) {}

        uint64_t start_tick = tick_count;
        start_sec = rtc::seconds();
        while (rtc::seconds() == start_sec) {}

        real_freq = (uint32_t)(tick_count - start_tick);
    }

    static void set_frequency(uint32_t hz)
    {
        if (hz == 0)
            hz = PIT_DEFAULT_HZ;

        pit_freq = hz;

        uint32_t divisor = PIT_BASE_FREQ / hz;
        if (divisor == 0)
            divisor = 1;
        if (divisor > 0xFFFF)
            divisor = 0xFFFF;

        // Recalculate actual frequency after clamping
        pit_freq = PIT_BASE_FREQ / divisor;

        ms_per_tick = 1000 / pit_freq;
        // Fractional part: (1000 % pit_freq) gives remainder per tick,
        // accumulate until >= pit_freq then add 1 ms
        remainder_step = 1000 % pit_freq;

        // Channel 0, lobyte/hibyte, rate generator (mode 2)
        port::byte_out(PIT_COMMAND, 0x34);
        port::byte_out(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
        port::byte_out(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    }

    void handler()
    {
        tick_count++;

        // Accumulate milliseconds with fractional correction
        uptime += ms_per_tick;
        tick_remainder += remainder_step;
        if (tick_remainder >= pit_freq)
        {
            uptime++;
            tick_remainder -= pit_freq;
        }

        // Cursor blinking - call every tick, screen controls the rate
        screen::update_cursor();

        // Flush back buffer to VRAM ~45 fps
        if (tick_count % 22 == 0)
            screen::flush();
    }

    void init()
    {
        init(PIT_DEFAULT_HZ);
    }

    void init(uint32_t frequency_hz)
    {
        tick_count = 0;
        uptime = 0;
        tick_remainder = 0;
        set_frequency(frequency_hz);
    }

    uint64_t uptime_ms()
    {
        return (tick_count * 1000) / real_freq;
    }

    uint64_t ticks()
    {
        return tick_count;
    }

    void sleep_ms(uint32_t ms)
    {
        uint64_t target_ticks = tick_count + ((uint64_t)ms * real_freq) / 1000;
        while (tick_count < target_ticks)
            asm volatile("hlt");
    }

    uint32_t frequency()
    {
        return pit_freq;
    }

    uint32_t real_frequency()
    {
        return real_freq;
    }
}