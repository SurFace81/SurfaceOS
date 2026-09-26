#ifndef SFOS_APP_H
#define SFOS_APP_H

#include "table.h"

// The running program itself, as the system describes it to SfMain.
//
//   Name      the program's name (its file name in /apps), NUL-terminated.
//   ArgCount  how many strings Args has.
//   Args      the command line: Args[0] is the program as it was named,
//             Args[1] and on its arguments. A path in the command line is
//             opened by the console for the program: Files->Open with
//             "argN:" opens what Args[N] names (a folder: "argN:/file").
//
// More fields are added at the end in later revisions.
typedef struct SfApp
{
    SfTableHeader      Hdr;
    const char*        Name;
    uint64_t           ArgCount;
    const char* const* Args;
} SfApp;

#define SF_APP_SIGNATURE    SF_SIGNATURE('S', 'F', 'A', 'P', 'P', 0, 0, 0)
#define SF_APP_REVISION     SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfApp, Name) == 16, "SfApp layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfApp, Args) == 32, "SfApp layout");
SF_STATIC_ASSERT(sizeof(SfApp) == 40, "SfApp layout");

#endif // SFOS_APP_H
