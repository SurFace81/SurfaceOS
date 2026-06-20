#ifndef ABI_TIME_H
#define ABI_TIME_H

#include "types.h"

struct uptime_t
{
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;
    uint64_t total_ms;
};

struct datetime_t
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    uint8_t  weekday;
};

#endif