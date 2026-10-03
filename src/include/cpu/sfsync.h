#ifndef SFSYNC_H
#define SFSYNC_H

// Synchronisation through the SurfaceOS SDK: events (kernel objects the
// program holds handles to), waiting on any waitable handle or for the
// first of several (WaitAny), and closing any handle. The SDK's mutex is
// built on an auto-reset event in the runtime.

namespace sfsync
{
    // Register the SFCALL_EVENT_*, SFCALL_WAIT, SFCALL_WAIT_ANY and
    // SFCALL_CLOSE handlers.
    void init();
}

#endif // SFSYNC_H
