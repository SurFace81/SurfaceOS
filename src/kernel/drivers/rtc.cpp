#include "../../include/drivers/rtc.h"

static uint8_t cmos_read(uint8_t reg)
{
    port::byte_out(CMOS_ADDRESS, reg);
    return port::byte_in(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Wait until RTC update-in-progress flag clears
static void wait_ready()
{
    while (cmos_read(RTC_REG_STATUS_A) & 0x80)
        ;
}

// Read raw values and convert from BCD if needed
static void read_raw(rtc_time* t)
{
    wait_ready();

    uint8_t raw_sec  = cmos_read(RTC_REG_SECONDS);
    uint8_t raw_min  = cmos_read(RTC_REG_MINUTES);
    uint8_t raw_hour = cmos_read(RTC_REG_HOURS);
    uint8_t raw_day  = cmos_read(RTC_REG_DAY);
    uint8_t raw_mon  = cmos_read(RTC_REG_MONTH);
    uint8_t raw_year = cmos_read(RTC_REG_YEAR);
    uint8_t raw_wday = cmos_read(RTC_REG_WEEKDAY);

    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);

    // If BCD mode (bit 2 of status B is clear), convert to binary
    if (!(status_b & 0x04))
    {
        raw_sec  = bcd_to_bin(raw_sec);
        raw_min  = bcd_to_bin(raw_min);
        raw_hour = bcd_to_bin(raw_hour & 0x7F) | (raw_hour & 0x80);
        raw_day  = bcd_to_bin(raw_day);
        raw_mon  = bcd_to_bin(raw_mon);
        raw_year = bcd_to_bin(raw_year);
        raw_wday = bcd_to_bin(raw_wday);
    }

    // Convert 12-hour to 24-hour if needed
    if (!(status_b & 0x02) && (raw_hour & 0x80))
    {
        raw_hour = ((raw_hour & 0x7F) + 12) % 24;
    }

    t->seconds = raw_sec;
    t->minutes = raw_min;
    t->hours   = raw_hour;
    t->day     = raw_day;
    t->month   = raw_mon;
    t->year    = 2000 + raw_year;
    t->weekday = raw_wday;
}

namespace rtc
{
    void init()
    {
        // RTC is always running, nothing special needed
        // Just verify we can read it
    }

    void read(rtc_time* t)
    {
        // Read twice and compare to avoid mid-update reads
        rtc_time a, b;
        do {
            read_raw(&a);
            read_raw(&b);
        } while (a.seconds != b.seconds ||
                 a.minutes != b.minutes ||
                 a.hours   != b.hours);

        *t = a;
    }

    uint8_t seconds()
    {
        rtc_time t;
        read(&t);
        return t.seconds;
    }

    uint8_t minutes()
    {
        rtc_time t;
        read(&t);
        return t.minutes;
    }

    uint8_t hours()
    {
        rtc_time t;
        read(&t);
        return t.hours;
    }

    uint8_t day()
    {
        rtc_time t;
        read(&t);
        return t.day;
    }

    uint8_t month()
    {
        rtc_time t;
        read(&t);
        return t.month;
    }

    uint16_t year()
    {
        rtc_time t;
        read(&t);
        return t.year;
    }
}