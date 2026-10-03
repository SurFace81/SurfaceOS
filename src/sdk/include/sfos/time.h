#ifndef SFOS_TIME_H
#define SFOS_TIME_H

#include "table.h"

/// A date and time of the machine's clock.
typedef struct SfDateTime
{
    uint16_t Year;          ///< 2026
    uint8_t  Month;         ///< 1..12
    uint8_t  Day;           ///< 1..31
    uint8_t  Hour;          ///< 0..23
    uint8_t  Minute;        ///< 0..59
    uint8_t  Second;        ///< 0..59
    uint8_t  Reserved;
} SfDateTime;

/// Time: the clock, the uptime and sleeping.
typedef struct SfTime SfTime;

struct SfTime
{
    SfTableHeader Hdr;

    /// *Time gets the date and time of the machine's clock.
    SfStatus (*GetTime)(SfTime* This, SfDateTime* Time);

    /// *Milliseconds gets the time since the system started: for
    /// measuring how long something takes.
    SfStatus (*GetUptime)(SfTime* This, uint64_t* Milliseconds);

    /// Does nothing for Milliseconds.
    ///
    /// SF_ABORTED when it was cut short.
    SfStatus (*Sleep)(SfTime* This, uint64_t Milliseconds);
};

#define SF_TIME_SIGNATURE   SF_SIGNATURE('S', 'F', 'T', 'I', 'M', 'E', 0, 0)

SF_STATIC_ASSERT(sizeof(SfDateTime) == 8, "SfDateTime layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfTime, GetTime) == 16, "SfTime layout");
SF_STATIC_ASSERT(sizeof(SfTime) == 40, "SfTime layout");

#endif // SFOS_TIME_H
