#ifndef SFOS_PROCESS_H
#define SFOS_PROCESS_H

#include "table.h"

// Processes: running programs, each with its own number (Id).
//
//   GetId    *Id gets the number of the calling process.
//   GetArgs  the command line of process Id: its strings one after another,
//            each NUL-terminated, into Buffer. *Size in: Buffer's size; out:
//            the bytes the strings take. SF_BUFFER_TOO_SMALL (with *Size
//            set) when they do not fit. *Count, when Count is not null,
//            gets how many strings there are. SF_NOT_FOUND for no such
//            process.
//   Start    start program Name (from /apps) with ArgCount strings Args as
//            its arguments - it sees Args[0] as its App->Args[1]. It shares
//            this program's screen (Ctrl+C reaches both) and runs on when
//            this program ends. *Handle refers to it for Wait. SF_NOT_FOUND
//            for no such program.
//
//            Flags SF_START_GIVE_INPUT: when this program owns its screen's
//            input (the keys), the new one gets it until it ends - then it
//            comes back. Without it, or from a program that does not own
//            the input, a ReadLine of the new program waits for its turn.
//   Wait     wait until the program behind Handle has ended; *Status (when
//            Status is not null) gets what it returned - SF_ABORTED when it
//            was stopped short (a fault, Ctrl+C). Handle is used up.
typedef struct SfProcess SfProcess;

struct SfProcess
{
    SfTableHeader Hdr;
    SfStatus (*GetId)(SfProcess* This, uint64_t* Id);
    SfStatus (*GetArgs)(SfProcess* This, uint64_t Id, char* Buffer, uint64_t* Size,
                        uint64_t* Count);
    SfStatus (*Start)(SfProcess* This, const char* Name, uint64_t ArgCount,
                      const char* const* Args, uint64_t Flags, uint64_t* Handle);
    SfStatus (*Wait)(SfProcess* This, uint64_t Handle, SfStatus* Status);
};

// Start flags
#define SF_START_GIVE_INPUT     0x1

#define SF_PROCESS_SIGNATURE    SF_SIGNATURE('S', 'F', 'P', 'R', 'O', 'C', 0, 0)
#define SF_PROCESS_REVISION     SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfProcess, GetArgs) == 24, "SfProcess layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfProcess, Start) == 32, "SfProcess layout");
SF_STATIC_ASSERT(sizeof(SfProcess) == 48, "SfProcess layout");

#endif // SFOS_PROCESS_H
