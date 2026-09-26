#ifndef SFTIME_H
#define SFTIME_H

// Time through the SurfaceOS SDK (SfTime): the RTC's date and time, the
// time since boot and sleeping.

namespace sftime
{
    // Register the SFCALL_TIME_* handlers.
    void init();
}

#endif // SFTIME_H
