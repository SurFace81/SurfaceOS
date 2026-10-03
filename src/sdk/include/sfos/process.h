#ifndef SFOS_PROCESS_H
#define SFOS_PROCESS_H

#include "table.h"
#include "file.h"

/// Processes: running programs, each with its own number (Id).
typedef struct SfProcess SfProcess;

struct SfProcess
{
    SfTableHeader Hdr;

    /// *Id gets the number of the calling process.
    SfStatus (*GetId)(SfProcess* This, uint64_t* Id);

    /// Copies the command line of process Id into Buffer: its strings
    /// one after another, each NUL-terminated.
    ///
    /// *Size in: Buffer's size; out: the bytes the strings take.
    /// SF_BUFFER_TOO_SMALL (with *Size set) when they do not fit. *Count, when
    /// Count is not null, gets how many strings there are. SF_NOT_FOUND for no
    /// such process.
    SfStatus (*GetArgs)(SfProcess* This, uint64_t Id, char* Buffer, uint64_t* Size,
                        uint64_t* Count);

    /// Starts program Name (from /apps, or a path with a root, such as
    /// "disk:/tools/x") with ArgCount strings Args as its arguments - it sees
    /// Args[0] as its App->Args[1].
    ///
    /// ArgFiles, when not null, has ArgCount entries: ArgFiles[i], when not
    /// null, is the file or folder argument i names, which the new program
    /// gets as its root arg<i+1>: (files.h). It shares this program's screen
    /// and runs on when this program ends. *Handle refers to it for Wait; it
    /// may be null when nobody is going to Wait. Flags are SF_START_*.
    /// SF_NOT_FOUND for no such program.
    SfStatus (*Start)(SfProcess* This, const char* Name, uint64_t ArgCount,
                      const char* const* Args, SfFile* const* ArgFiles, uint64_t Flags,
                      uint64_t* Handle);

    /// Waits until the program behind Handle has ended; *Status (when
    /// Status is not null) gets what it returned - SF_ABORTED when it was
    /// stopped short (Ctrl+Alt+C, EndProcess), SF_CRASHED when a CPU exception
    /// ended it (the kernel says which on its screen).
    ///
    /// Handle is used up.
    SfStatus (*Wait)(SfProcess* This, uint64_t Handle, SfStatus* Status);

    /// *Id gets the number of the program behind Handle (sfos/admin.h
    /// speaks of programs by number).
    SfStatus (*IdOf)(SfProcess* This, uint64_t Handle, uint64_t* Id);
};

/// Start: when this program owns its screen's input (the keys), the new
/// one gets it until it ends - then it comes back.
///
/// Without it, or from a program that does not own the input, a ReadLine of
/// the new program waits for its turn.
#define SF_START_GIVE_INPUT     0x1
/// Start: the program runs on a hidden screen of its own instead, what
/// it prints logged to console_<date>_<time>.log in its data folder (as the
/// console's `&`).
#define SF_START_BACKGROUND     0x2
/// Start: with the admin right (sfos/admin.h) - for a program that has
/// it.
#define SF_START_ADMIN          0x4
/// Start: ArgFiles is not null and has one entry more, a file open for
/// writing: what the program prints in SF_CONSOLE_LINE goes there, from that
/// file's position on, instead of to the screen (the console's `>`).
#define SF_START_OUTPUT         0x8
/// Start: ArgFiles is not null and has one entry more still (after the
/// output file, if any), a file open for reading: ReadLine gives its lines,
/// from that file's position on, instead of what is typed (the console's `|`).
#define SF_START_INPUT          0x10

#define SF_PROCESS_SIGNATURE    SF_SIGNATURE('S', 'F', 'P', 'R', 'O', 'C', 0, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfProcess, GetArgs) == 24, "SfProcess layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfProcess, Start) == 32, "SfProcess layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfProcess, IdOf) == 48, "SfProcess layout");
SF_STATIC_ASSERT(sizeof(SfProcess) == 56, "SfProcess layout");

#endif // SFOS_PROCESS_H
