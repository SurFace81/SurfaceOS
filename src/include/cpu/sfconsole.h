#ifndef SFCONSOLE_H
#define SFCONSOLE_H

// The console through the SurfaceOS SDK (SfConsole): printing, reading a
// line or a key, and drawing cells on the program's own screen.

namespace sfconsole
{
    // Register the SFCALL_CONSOLE_* handlers.
    void init();
}

#endif // SFCONSOLE_H
