#ifndef SFOS_SYSTEM_H
#define SFOS_SYSTEM_H

#include "table.h"
#include "app.h"
#include "console.h"

// The system table: everything a program can ask of the system, handed to
// SfMain. Services are tables of their own, reached from here.
//
//   Console   the program's console.
//
// More services (Memory, Time, Process, ...) are added at the end in later
// revisions; check Hdr.Size (SF_HAS_FIELD) before using one that came
// later than the revision a program needs.
typedef struct SfSystem
{
    SfTableHeader Hdr;
    SfConsole*    Console;
} SfSystem;

#define SF_SYSTEM_SIGNATURE SF_SIGNATURE('S', 'F', 'S', 'Y', 'S', 'T', 'E', 'M')
#define SF_SYSTEM_REVISION  SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfSystem, Console) == 16, "SfSystem layout");
SF_STATIC_ASSERT(sizeof(SfSystem) == 24, "SfSystem layout");

// The program's entry point. What it returns becomes the exit status its
// parent sees.
#ifdef __cplusplus
extern "C"
#endif
SfStatus SfMain(SfApp* App, SfSystem* Sys);

#endif // SFOS_SYSTEM_H
