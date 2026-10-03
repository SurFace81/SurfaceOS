#ifndef SFCONSOLE_H
#define SFCONSOLE_H

// The console through the SurfaceOS SDK (SfConsole): printing, reading a
// line or a key, drawing cells on the program's own screen, and the
// clipboard.

namespace sfconsole
{
    // Register the SFCALL_CONSOLE_* handlers.
    void init();

    // Is a key there for the caller - one ReadKey would report, while it
    // owns its screen's input (SfSync WaitAny)? Passes over the others.
    bool key_ready();
}

#endif // SFCONSOLE_H
