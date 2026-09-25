#ifndef SFOS_APP_H
#define SFOS_APP_H

#include "table.h"

// The running program itself, as the kernel describes it to SfMain.
//
//   Name   the program's name (its file name in /apps), NUL-terminated.
//
// More fields (the arguments) are added at the end in later revisions.
typedef struct SfApp
{
    SfTableHeader Hdr;
    const char*   Name;
} SfApp;

#define SF_APP_SIGNATURE    SF_SIGNATURE('S', 'F', 'A', 'P', 'P', 0, 0, 0)
#define SF_APP_REVISION     SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfApp, Name) == 16, "SfApp layout");
SF_STATIC_ASSERT(sizeof(SfApp) == 24, "SfApp layout");

#endif // SFOS_APP_H
