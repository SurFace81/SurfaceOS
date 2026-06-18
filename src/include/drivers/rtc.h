#ifndef RTC_H
#define RTC_H

#include "../cpu/types.h"
#include "../cpu/ports.h"

// CMOS ports
#define CMOS_ADDRESS    0x70
#define CMOS_DATA       0x71

// CMOS RTC registers
#define RTC_REG_SECONDS     0x00
#define RTC_REG_MINUTES     0x02
#define RTC_REG_HOURS       0x04
#define RTC_REG_WEEKDAY     0x06
#define RTC_REG_DAY         0x07
#define RTC_REG_MONTH       0x08
#define RTC_REG_YEAR        0x09
#define RTC_REG_STATUS_A    0x0A
#define RTC_REG_STATUS_B    0x0B

struct rtc_time
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t weekday;
};

namespace rtc
{
    void init();

    // Read current time from CMOS
    void read(rtc_time* t);
    // Write time to CMOS RTC
    void write(const rtc_time* t);

    // Individual field readers
    uint8_t seconds();
    uint8_t minutes();
    uint8_t hours();
    uint8_t day();
    uint8_t month();
    uint16_t year();
}

#endif