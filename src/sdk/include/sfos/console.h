#ifndef SFOS_CONSOLE_H
#define SFOS_CONSOLE_H

#include "table.h"

// The program's console: its screen and keyboard.
//
//   Print     write Text (UTF-8, NUL-terminated) at the cursor.
//   ReadLine  read one line typed by the user into Buffer (Size bytes,
//             including the terminating NUL; the line break is not stored).
//             *Length gets the line's length. SF_ABORTED when the user
//             pressed Ctrl+C instead, SF_END_OF_FILE when there is no more
//             input.
typedef struct SfConsole SfConsole;

struct SfConsole
{
    SfTableHeader Hdr;
    SfStatus (*Print)(SfConsole* This, const char* Text);
    SfStatus (*ReadLine)(SfConsole* This, char* Buffer, uint64_t Size, uint64_t* Length);
};

#define SF_CONSOLE_SIGNATURE    SF_SIGNATURE('S', 'F', 'C', 'O', 'N', 'S', 'O', 'L')
#define SF_CONSOLE_REVISION     SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, Print) == 16, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, ReadLine) == 24, "SfConsole layout");
SF_STATIC_ASSERT(sizeof(SfConsole) == 32, "SfConsole layout");

#endif // SFOS_CONSOLE_H
