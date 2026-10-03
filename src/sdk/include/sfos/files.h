#ifndef SFOS_FILES_H
#define SFOS_FILES_H

#include "table.h"
#include "file.h"

/// The program's roots.
///
/// A path names a root and a place below it:
///
///   data:/notes.txt   the program's own folder (the system keeps one per
///                     program and creates it on first start)
///   tmp:/job_1        shared temporary files, emptied at every boot; its
///                     contents cannot be listed (tmp:/ itself does not
///                     open) - make a name nobody else uses with
///                     CreateUnique
///   arg1:             what a path in the command line names (SfApp Args)
///
/// No path leads above its root.
typedef struct SfFiles SfFiles;

struct SfFiles
{
    SfTableHeader Hdr;

    /// Opens Path with SF_FILE_* Mode; *Out gets the new SfFile.
    ///
    /// "data:/" alone opens the root folder itself, to open files relative to
    /// it. SF_INVALID_PARAMETER when Path has no root, SF_NOT_FOUND for an
    /// unknown root or a missing file, SF_ACCESS_DENIED when Path leads above
    /// its root.
    SfStatus (*Open)(SfFiles* This, const char* Path, uint64_t Mode, SfFile** Out);

    /// Creates a new empty file in tmp:/ under a name no other file
    /// has, open for reading and writing.
    ///
    /// *Out gets it; Path, when not null, gets its path ("tmp:/...") in
    /// PathSize bytes.
    SfStatus (*CreateUnique)(SfFiles* This, SfFile** Out, char* Path, uint64_t PathSize);

    /// Makes folder Path; SF_ALREADY_EXISTS when there is something by
    /// that name.
    SfStatus (*CreateDirectory)(SfFiles* This, const char* Path);

    /// Removes file Path, or folder Path when it is empty (SF_IN_USE
    /// when it is not).
    SfStatus (*Delete)(SfFiles* This, const char* Path);

    /// Moves OldPath to NewPath, which must not exist; both on the same
    /// volume (SF_ACCESS_DENIED otherwise - copy instead).
    SfStatus (*Rename)(SfFiles* This, const char* OldPath, const char* NewPath);
};

#define SF_FILES_SIGNATURE  SF_SIGNATURE('S', 'F', 'F', 'I', 'L', 'E', 'S', 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfFiles, Open) == 16, "SfFiles layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfFiles, CreateUnique) == 24, "SfFiles layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfFiles, Rename) == 48, "SfFiles layout");
SF_STATIC_ASSERT(sizeof(SfFiles) == 56, "SfFiles layout");

#endif // SFOS_FILES_H
