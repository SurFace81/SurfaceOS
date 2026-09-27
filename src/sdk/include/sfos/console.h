#ifndef SFOS_CONSOLE_H
#define SFOS_CONSOLE_H

#include "table.h"
#include "keys.h"

// The program's console: its screen and keyboard.
//
// The screen is a grid of cells, Columns x Rows, (0, 0) at the top left;
// the system's title bar above it is not part of it. Keys reach the program
// that owns its screen's input (sfos/process.h, Start); anyone else waits
// in ReadLine or ReadKey for its turn.
//
// Two modes (SetMode):
//   SF_CONSOLE_LINE  the default: a scrolling text console. Print wraps and
//                    scrolls, ReadLine edits a line; Ctrl+C there ends it
//                    with SF_ABORTED.
//   SF_CONSOLE_RAW   the program draws the screen itself: switching to it
//                    clears the screen and hides the cursor, keys come only
//                    through ReadKey - Ctrl+C too - and ReadLine is
//                    SF_UNSUPPORTED. Back to SF_CONSOLE_LINE clears it again.
//
//   Print      write Text (NUL-terminated) at the cursor, in the colours of
//              SetColor; wraps and scrolls.
//   ReadLine   read one line typed by the user into Buffer (Size bytes,
//              including the terminating NUL; the line break is not stored,
//              a longer line is cut). *Length, when Length is not null, gets
//              the stored length. SF_ABORTED when the user pressed Ctrl+C
//              instead, SF_END_OF_FILE after Ctrl+D on an empty line.
//   GetSize    *Columns and *Rows get the size of the screen.
//   SetCursor  put the cursor at (Column, Row) - where Print goes on - and
//              show it (Visible != 0) or hide it.
//   SetColor   the colours Print and WriteAt use from now on: SF_COLOR_*.
//   WriteAt    write Text at (Column, Row) in the current colours. It does
//              not wrap (the rest of a long text is cut at the edge) and
//              leaves the cursor where it is.
//   Draw       fill a Width x Height rectangle at (Column, Row) with Cells,
//              row by row; what falls outside the screen is cut.
//   ReadKey    wait for a key and describe it in *Key (sfos/keys.h). In
//              SF_CONSOLE_LINE, Ctrl+C gives SF_ABORTED instead.
//   SetMode    SF_CONSOLE_LINE or SF_CONSOLE_RAW, see above.
//   SetTitle   the program's own words in the title bar, after its name
//              (a file name, say); an empty Text clears them.
//   Clear      blank the screen in the current colours and put the cursor
//              at (0, 0).
//   WaitInput  wait until this program owns its screen's input: the
//              console waits so for the program it started to end, or to
//              be paused (Ctrl+Alt+Z hands the keys back to the console),
//              or to move away (fg, bg). SF_ABORTED when the
//              program is being ended instead.
//
// ReadLine and ReadKey also end with SF_ABORTED when another program on the
// screen takes the input while they wait for a key (Ctrl+Alt+Z letting a
// paused program go on). A program moved to another screen (fg, bg) just
// waits for its turn there.
typedef struct SfConsole SfConsole;

// Colours: 0..7, and their bright forms 8..15.
#define SF_COLOR_BLACK          0
#define SF_COLOR_RED            1
#define SF_COLOR_GREEN          2
#define SF_COLOR_YELLOW         3
#define SF_COLOR_BLUE           4
#define SF_COLOR_MAGENTA        5
#define SF_COLOR_CYAN           6
#define SF_COLOR_WHITE          7
#define SF_COLOR_BRIGHT         8       // SF_COLOR_BRIGHT | SF_COLOR_RED...

// SfCell.Color: foreground in the low four bits, background in the high.
#define SF_CELL_COLOR(Fg, Bg)   ((uint8_t)(((Bg) << 4) | (Fg)))

#define SF_CONSOLE_LINE         0
#define SF_CONSOLE_RAW          1

typedef struct SfCell
{
    char    Char;
    uint8_t Color;              // SF_CELL_COLOR
} SfCell;

typedef struct SfKey
{
    uint16_t Code;              // SF_KEY_*
    uint8_t  Mods;              // SF_MOD_*
    char     Char;              // what it types, 0 for nothing
} SfKey;

struct SfConsole
{
    SfTableHeader Hdr;
    SfStatus (*Print)(SfConsole* This, const char* Text);
    SfStatus (*ReadLine)(SfConsole* This, char* Buffer, uint64_t Size, uint64_t* Length);
    SfStatus (*GetSize)(SfConsole* This, uint32_t* Columns, uint32_t* Rows);
    SfStatus (*SetCursor)(SfConsole* This, uint32_t Column, uint32_t Row, uint8_t Visible);
    SfStatus (*SetColor)(SfConsole* This, uint8_t Foreground, uint8_t Background);
    SfStatus (*WriteAt)(SfConsole* This, uint32_t Column, uint32_t Row, const char* Text);
    SfStatus (*Draw)(SfConsole* This, uint32_t Column, uint32_t Row, uint32_t Width,
                     uint32_t Height, const SfCell* Cells);
    SfStatus (*ReadKey)(SfConsole* This, SfKey* Key);
    SfStatus (*SetMode)(SfConsole* This, uint64_t Mode);
    SfStatus (*SetTitle)(SfConsole* This, const char* Text);
    SfStatus (*Clear)(SfConsole* This);
    SfStatus (*WaitInput)(SfConsole* This);
};

#define SF_CONSOLE_SIGNATURE    SF_SIGNATURE('S', 'F', 'C', 'O', 'N', 'S', 'O', 'L')
#define SF_CONSOLE_REVISION     SF_REVISION(1, 0)

SF_STATIC_ASSERT(sizeof(SfCell) == 2, "SfCell layout");
SF_STATIC_ASSERT(sizeof(SfKey) == 4, "SfKey layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, Print) == 16, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, ReadLine) == 24, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, GetSize) == 32, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, SetTitle) == 88, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, Clear) == 96, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, WaitInput) == 104, "SfConsole layout");
SF_STATIC_ASSERT(sizeof(SfConsole) == 112, "SfConsole layout");

#endif // SFOS_CONSOLE_H
