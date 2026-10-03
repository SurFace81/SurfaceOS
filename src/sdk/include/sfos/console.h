#ifndef SFOS_CONSOLE_H
#define SFOS_CONSOLE_H

#include "table.h"
#include "keys.h"

// Colours: 0..7, and their bright forms 8..15.

/// Colour 0: black.
#define SF_COLOR_BLACK          0
/// Colour 1: red.
#define SF_COLOR_RED            1
/// Colour 2: green.
#define SF_COLOR_GREEN          2
/// Colour 3: yellow (brown when not bright).
#define SF_COLOR_YELLOW         3
/// Colour 4: blue.
#define SF_COLOR_BLUE           4
/// Colour 5: magenta.
#define SF_COLOR_MAGENTA        5
/// Colour 6: cyan.
#define SF_COLOR_CYAN           6
/// Colour 7: white (light grey when not bright).
#define SF_COLOR_WHITE          7
/// Makes a colour bright: SF_COLOR_BRIGHT | SF_COLOR_RED...
#define SF_COLOR_BRIGHT         8

/// SfCell.Color: foreground in the low four bits, background in the
/// high.
#define SF_CELL_COLOR(Fg, Bg)   ((uint8_t)(((Bg) << 4) | (Fg)))

/// SetMode: a scrolling text console, Print and ReadLine (the default).
#define SF_CONSOLE_LINE         0
/// SetMode: the program draws the screen itself, keys only through
/// ReadKey.
#define SF_CONSOLE_RAW          1

/// The most bytes the clipboard holds.
#define SF_CLIPBOARD_SIZE       (64 * 1024)

/// One character cell of the screen, for Draw.
typedef struct SfCell
{
    char    Char;               ///< ASCII, or a character of sfos/chars.h (code page 437)
    uint8_t Color;              ///< SF_CELL_COLOR
} SfCell;

/// A key, as ReadKey reports it.
typedef struct SfKey
{
    uint16_t Code;              ///< SF_KEY_*
    uint8_t  Mods;              ///< SF_MOD_*
    char     Char;              ///< what it types, 0 for nothing
} SfKey;

/// The program's console: its screen and keyboard, and the clipboard
/// shared by all programs and screens.
///
/// The screen is a grid of cells, Columns x Rows, (0, 0) at the top left;
/// the system's title bar above it is not part of it. Keys reach the program
/// that owns its screen's input (sfos/process.h, Start); anyone else waits
/// in ReadLine or ReadKey for its turn. ReadLine and ReadKey also end with
/// SF_ABORTED when another program on the screen takes the input while they
/// wait (Ctrl+Alt+Z letting a paused program go on). A program moved to
/// another screen (fg, bg) just waits for its turn there.
///
/// Two modes (SetMode):
///   SF_CONSOLE_LINE  the default: a scrolling text console. Print wraps and
///                    scrolls, ReadLine edits a line; Ctrl+C there ends it
///                    with SF_ABORTED.
///   SF_CONSOLE_RAW   the program draws the screen itself: switching to it
///                    clears the screen and hides the cursor, keys come only
///                    through ReadKey - Ctrl+C too - and ReadLine is
///                    SF_UNSUPPORTED. Back to SF_CONSOLE_LINE clears it again.
typedef struct SfConsole SfConsole;

struct SfConsole
{
    SfTableHeader Hdr;

    /// Writes Text (NUL-terminated) at the cursor in the colours of
    /// SetColor; wraps and scrolls.
    SfStatus (*Print)(SfConsole* This, const char* Text);

    /// Reads one line typed by the user into Buffer (Size bytes,
    /// including the terminating NUL; the line break is not stored, a longer
    /// line is cut).
    ///
    /// *Length, when Length is not null, gets the stored length. SF_ABORTED
    /// when the user pressed Ctrl+C instead, SF_END_OF_FILE after Ctrl+D on an
    /// empty line.
    ///
    /// Up and Down bring back the program's last 16 lines. Ctrl+arrows
    /// select text on the screen from the cursor on, Ctrl+C copies it to the
    /// clipboard (only without a selection does it end the line), Esc or any
    /// other key drops it; Ctrl+V types what the clipboard holds, its line
    /// breaks as spaces. A program started with SF_START_INPUT
    /// (sfos/process.h) reads the lines of its input file instead, without
    /// waiting for keys, and SF_END_OF_FILE after the last.
    SfStatus (*ReadLine)(SfConsole* This, char* Buffer, uint64_t Size, uint64_t* Length);

    /// *Columns and *Rows get the size of the screen.
    SfStatus (*GetSize)(SfConsole* This, uint32_t* Columns, uint32_t* Rows);

    /// Puts the cursor at (Column, Row) - where Print goes on - and
    /// shows it (Visible != 0) or hides it.
    SfStatus (*SetCursor)(SfConsole* This, uint32_t Column, uint32_t Row, uint8_t Visible);

    /// Sets the colours Print and WriteAt use from now on: SF_COLOR_*.
    SfStatus (*SetColor)(SfConsole* This, uint8_t Foreground, uint8_t Background);

    /// Writes Text at (Column, Row) in the current colours.
    ///
    /// It does not wrap (the rest of a long text is cut at the edge) and
    /// leaves the cursor where it is.
    SfStatus (*WriteAt)(SfConsole* This, uint32_t Column, uint32_t Row, const char* Text);

    /// Fills a Width x Height rectangle at (Column, Row) with Cells,
    /// row by row; what falls outside the screen is cut.
    SfStatus (*Draw)(SfConsole* This, uint32_t Column, uint32_t Row, uint32_t Width,
                     uint32_t Height, const SfCell* Cells);

    /// Waits for a key and describes it in *Key (sfos/keys.h).
    ///
    /// In SF_CONSOLE_LINE, Ctrl+C gives SF_ABORTED instead.
    SfStatus (*ReadKey)(SfConsole* This, SfKey* Key);

    /// Switches to SF_CONSOLE_LINE or SF_CONSOLE_RAW (see SfConsole).
    SfStatus (*SetMode)(SfConsole* This, uint64_t Mode);

    /// Sets the program's own words in the title bar, after its name (a
    /// file name, say); an empty Text clears them.
    SfStatus (*SetTitle)(SfConsole* This, const char* Text);

    /// Blanks the screen in the current colours and puts the cursor at
    /// (0, 0).
    SfStatus (*Clear)(SfConsole* This);

    /// Waits until this program owns its screen's input: the console
    /// waits so for the program it started to end, or to be paused (Ctrl+Alt+Z
    /// hands the keys back to the console), or to move away (fg, bg).
    ///
    /// SF_ABORTED when the program is being ended instead.
    SfStatus (*WaitInput)(SfConsole* This);

    /// Sets the words ReadLine suggests while a word is typed: the rest
    /// of the first one that starts with it shows dimmed after the cursor, and
    /// Tab (or Right at the end of the line) takes it.
    ///
    /// Commands (for the first word, and one after `|` or `&`) and Names (for
    /// the others) are lists of words, one a line; a word with '/' in it gets
    /// none. Kept until the next call; null or "" for none.
    SfStatus (*SetHints)(SfConsole* This, const char* Commands, const char* Names);

    /// Puts Size bytes of Data on the clipboard (at most
    /// SF_CLIPBOARD_SIZE; SF_BUFFER_TOO_SMALL for more).
    ///
    /// Size 0 empties it.
    SfStatus (*SetClipboard)(SfConsole* This, const void* Data, uint64_t Size);

    /// Copies what is on the clipboard into Buffer.
    ///
    /// *Size in: Buffer's size; out: how many bytes it holds.
    /// SF_BUFFER_TOO_SMALL (with *Size set) when they do not fit.
    SfStatus (*GetClipboard)(SfConsole* This, void* Buffer, uint64_t* Size);
};

#define SF_CONSOLE_SIGNATURE    SF_SIGNATURE('S', 'F', 'C', 'O', 'N', 'S', 'O', 'L')

SF_STATIC_ASSERT(sizeof(SfCell) == 2, "SfCell layout");
SF_STATIC_ASSERT(sizeof(SfKey) == 4, "SfKey layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, Print) == 16, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, ReadLine) == 24, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, GetSize) == 32, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, SetTitle) == 88, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, Clear) == 96, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, WaitInput) == 104, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, SetHints) == 112, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, SetClipboard) == 120, "SfConsole layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfConsole, GetClipboard) == 128, "SfConsole layout");
SF_STATIC_ASSERT(sizeof(SfConsole) == 136, "SfConsole layout");

#endif // SFOS_CONSOLE_H
