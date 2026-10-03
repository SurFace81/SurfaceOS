#ifndef SFOS_SYSTEM_H
#define SFOS_SYSTEM_H

#include "table.h"
#include "app.h"
#include "console.h"
#include "files.h"
#include "memory.h"
#include "time.h"
#include "process.h"
#include "thread.h"
#include "sync.h"
#include "admin.h"

/// The system table, handed to SfMain: everything a program can ask of
/// the system.
///
/// Each service is a table of its own, reached from here. More services are
/// added at the end later; check Hdr.Size (SF_HAS_FIELD) before using one this
/// SDK does not have yet.
typedef struct SfSystem
{
    SfTableHeader Hdr;
    /// The program's console: its screen, keyboard and the clipboard.
    SfConsole*    Console;
    /// Files and folders under the program's roots: data:/, tmp:/,
    /// argN:.
    SfFiles*      Files;
    /// Pages and the heap.
    SfMemory*     Memory;
    /// The clock, the uptime and sleeping.
    SfTime*       Time;
    /// Starting other programs, waiting for them, command lines.
    SfProcess*    Process;
    /// Threads of this program.
    SfThread*     Thread;
    /// Mutexes, events and WaitAny between threads and programs.
    SfSync*       Sync;
    /// What only a program with the admin right can do (sfos/admin.h);
    /// null for any other program.
    SfAdmin*      Admin;
} SfSystem;

#define SF_SYSTEM_SIGNATURE SF_SIGNATURE('S', 'F', 'S', 'Y', 'S', 'T', 'E', 'M')

SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Console) == 16, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Files) == 24, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Memory) == 32, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Time) == 40, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Process) == 48, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Thread) == 56, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Sync) == 64, "SfSystem layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Admin) == 72, "SfSystem layout");
SF_STATIC_ASSERT(sizeof(SfSystem) == 80, "SfSystem layout");

#ifdef __cplusplus
extern "C" {
#endif

/// The program's entry point, written by the program.
///
/// The same in C and C++: C++ gets C linkage from this declaration, so no
/// extern "C" is needed. What it returns ends the program and is the status
/// its parent gets.
SfStatus SfMain(SfApp* App, SfSystem* Sys);

#ifdef __cplusplus
}
#endif

#endif // SFOS_SYSTEM_H
